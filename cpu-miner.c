/*
 * Copyright 2010 Jeff Garzik
 * Copyright 2012-2014 pooler
 * Copyright 2014 Lucas Jones
 * Copyright 2017 Elias Limneos
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */
 
#define XCODE_INCLUDED // if you compile as a library for Xcode

#include "cpuminer-config.h"
#define _GNU_SOURCE
#define __arm__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#ifdef WIN32
#include <windows.h>
#else
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#if HAVE_SYS_SYSCTL_H
#include <sys/types.h>
#if HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <sys/sysctl.h>
#endif
#endif
#include <jansson.h>
#include <curl/curl.h>
#include "compat.h"
#include "miner.h"
#include "notify.h"

 
static bool hasRegisteredForNotifications=0;
static char *realUserUrl=NULL;
static char *realUserUser=NULL;
static char *realUserPass=NULL;


#define PROGRAM_NAME	"MobileMiner"
#define LP_SCANTIME		60
#define JSON_BUF_LEN 345

 

#ifdef __linux /* Linux specific policy and affinity management */
#include <sched.h>
static inline void drop_policy(void) {
    struct sched_param param;
    param.sched_priority = 0;

#ifdef SCHED_IDLE
    if (unlikely(sched_setscheduler(0, SCHED_IDLE, &param) == -1))
#endif
#ifdef SCHED_BATCH
    sched_setscheduler(0, SCHED_BATCH, &param);
#endif
}

static inline void affine_to_cpu(int id, int cpu) {
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
}
#elif defined(__FreeBSD__) /* FreeBSD specific policy and affinity management */
#include <sys/cpuset.h>
static inline void drop_policy(void)
{
}

static inline void affine_to_cpu(int id, int cpu)
{
    cpuset_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof(cpuset_t), &set);
}
#else
static inline void drop_policy(void)
{
}

static inline void affine_to_cpu(int id, int cpu)
{
}
#endif

enum workio_commands {
    WC_GET_WORK, WC_SUBMIT_WORK,
};

struct workio_cmd {
    enum workio_commands cmd;
    struct thr_info *thr;
    union {
        struct work *work;
    } u;
};

enum algos {
    ALGO_SCRYPT,      /* scrypt(1024,1,1) */
    ALGO_SHA256D,     /* SHA-256d */
    ALGO_KECCAK,      /* Keccak */
    ALGO_HEAVY,       /* Heavy */
    ALGO_QUARK,       /* Quark */
    ALGO_SKEIN,       /* Skein */
    ALGO_SHAVITE3,    /* Shavite3 */
    ALGO_BLAKE,       /* Blake */
    ALGO_FRESH,       /* Fresh */
    ALGO_X11,         /* X11 */
    ALGO_X13,         /* X13 */
    ALGO_X14,         /* X14 */
    ALGO_X15,         /* X15 Whirlpool */
    ALGO_CRYPTONIGHT, /* CryptoNight */
};

static const char *algo_names[] = {
    [ALGO_SCRYPT] =      "scrypt",
    [ALGO_SHA256D] =     "sha256d",
    [ALGO_KECCAK] =      "keccak",
    [ALGO_HEAVY] =       "heavy",
    [ALGO_QUARK] =       "quark",
    [ALGO_SKEIN] =       "skein",
    [ALGO_SHAVITE3] =    "shavite3",
    [ALGO_BLAKE] =       "blake",
    [ALGO_FRESH] =       "fresh",
    [ALGO_X11] =         "x11",
    [ALGO_X13] =         "x13",
    [ALGO_X14] =         "x14",
    [ALGO_X15] =         "x15",
    [ALGO_CRYPTONIGHT] = "cryptonight",
};

bool opt_debug = false;
bool opt_protocol = false;
static bool opt_benchmark = false;
bool opt_redirect = true;
bool want_longpoll = true;
bool have_longpoll = false;
bool want_stratum = true;
bool have_stratum = false;
static bool submit_old = false;
bool use_syslog = false;
static bool opt_background = false;
static bool opt_quiet = false;
static int opt_retries = -1;
static int opt_fail_pause = 10;
bool jsonrpc_2 = false;
int opt_timeout = 0;
static int opt_scantime = 5;
static json_t *opt_config;
static const bool opt_time = true;
static enum algos opt_algo = ALGO_SCRYPT;
static int opt_scrypt_n = 1024;
static int opt_n_threads;
static int num_processors;
char *rpc_url;
char *rpc_userpass;
char *rpc_user, *rpc_pass;
char *opt_cert;
char *opt_proxy;
long opt_proxy_type;
struct thr_info *thr_info;
static int work_thr_id;
int longpoll_thr_id = -1;
int stratum_thr_id = -1;
struct work_restart *work_restart = NULL;
struct stratum_ctx stratum;
static char rpc2_id[64] = "";
static char *rpc2_blob = NULL;
static int rpc2_bloblen = 0;
static uint32_t rpc2_target = 0;
static char *rpc2_job_id = NULL;
bool aes_ni_supported = false;

pthread_mutex_t applog_lock;
static pthread_mutex_t stats_lock;
static pthread_mutex_t rpc2_job_lock;
static pthread_mutex_t rpc2_login_lock;

static unsigned long accepted_count = 0L;
static unsigned long rejected_count = 0L;
static double *thr_hashrates;


#ifdef HAVE_GETOPT_LONG
#include <getopt.h>
#else
struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};
#endif

static char *hwmodel=NULL;
static void printHashRate();


void notificationCallback(CFNotificationCenterRef center, void *observer, CFStringRef name, const void *object, CFDictionaryRef userInfo){

	const char *namechr = CFStringGetCStringPtr( name, 4 /*30*/ ) ;
	if (!namechr){
		namechr = CFStringGetCStringPtr( name, 30 ) ;
		if (!namechr){
			namechr = CFStringGetCStringPtr( name, 1) ;
		}
	}

	if (strstr(namechr,"HASHRATE")){
		printHashRate();
	}
	if (strstr(namechr,"STOP_MINING")){
		applog(LOG_INFO,"Received STOP signal. Stopping Threads...");
		should_stop_mining=1;
	}
	if (strstr(namechr,"kNetworkReachabilityChangedNotification")){
		if (rpc_user){
			//bool reachable=(bool)*(bool *)object;
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1.0 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
				connectedToInternet=[[objc_getClass("PCPersistentInterfaceManager") sharedInstance] isInternetReachable];
				applog(LOG_INFO,"New network state: %s",connectedToInternet ? "Connected" : "Disconnected");
			});
		}
	}
	
}

static char const usage[] =
        "\
Usage: " PROGRAM_NAME " [OPTIONS]\n\
Options:\n\
  -a, --algo=ALGO       specify the algorithm to use\n\
                          scrypt       scrypt(1024, 1, 1) (default)\n\
                          scrypt:N     scrypt(N, 1, 1)\n\
                          sha256d      SHA-256d\n\
                          keccak       Keccak\n\
                          quark        Quark\n\
                          heavy        Heavy\n\
                          skein        Skein\n\
                          shavite3     Shavite3\n\
                          blake        Blake\n\
                          fresh        Fresh\n\
                          x11          X11\n\
                          x13          X13\n\
                          x14          X14\n\
                          x15          X15\n\
                          cryptonight  CryptoNight\n\
  -o, --url=URL         URL of mining server\n\
  -O, --userpass=U:P    username:password pair for mining server\n\
  -u, --user=USERNAME   username for mining server\n\
  -p, --pass=PASSWORD   password for mining server\n\
      --cert=FILE       certificate for mining server using SSL\n\
  -x, --proxy=[PROTOCOL://]HOST[:PORT]  connect through a proxy\n\
  -t, --threads=N       number of miner threads (default: number of processors)\n\
  -r, --retries=N       number of times to retry if a network call fails\n\
                          (default: retry indefinitely)\n\
  -R, --retry-pause=N   time to pause between retries, in seconds (default: 30)\n\
  -T, --timeout=N       timeout for long polling, in seconds (default: none)\n\
  -s, --scantime=N      upper bound on time spent scanning current work when\n\
                          long polling is unavailable, in seconds (default: 5)\n\
      --no-longpoll     disable X-Long-Polling support\n\
      --no-stratum      disable X-Stratum support\n\
      --no-redirect     ignore requests to change the URL of the mining server\n\
  -q, --quiet           disable per-thread hashmeter output\n\
  -D, --debug           enable debug output\n\
  -P, --protocol-dump   verbose dump of protocol-level activities\n"
#ifdef HAVE_SYSLOG_H
        "\
  -S, --syslog          use system log for output messages\n"
#endif
#ifndef WIN32
        "\
  -B, --background      run the miner in the background\n"
#endif
        "\
      --benchmark       run in offline benchmark mode\n\
  -c, --config=FILE     load a JSON-format configuration file\n\
  -V, --version         display version information and exit\n\
  -h, --help            display this help text and exit\n\
";

static char short_options[] =
#ifndef WIN32
        "B"
#endif
#ifdef HAVE_SYSLOG_H
                "S"
#endif
        "a:c:Dhp:Px:qr:R:s:t:T:o:u:O:V";

static struct option options[] = {
        { "algo", 1, NULL, 'a' },
#ifndef WIN32
        { "background", 0, NULL, 'B' },
#endif
        { "benchmark", 0, NULL, 1005 },
        { "cert", 1, NULL, 1001 },
        { "config", 1, NULL, 'c' },
        { "debug", 0, NULL, 'D' },
        { "help", 0, NULL, 'h' },
        { "no-longpoll", 0, NULL, 1003 },
        { "no-redirect", 0, NULL, 1009 },
        { "no-stratum", 0, NULL, 1007 },
        { "pass", 1, NULL, 'p' },
        { "protocol-dump", 0, NULL, 'P' },
        { "proxy", 1, NULL, 'x' },
        { "quiet", 0, NULL, 'q' },
        { "retries", 1, NULL, 'r' },
        { "retry-pause", 1, NULL, 'R' },
        { "scantime", 1, NULL, 's' },
#ifdef HAVE_SYSLOG_H
        { "syslog", 0, NULL, 'S' },
#endif
        { "threads", 1, NULL, 't' },
        { "timeout", 1, NULL, 'T' },
        { "url", 1, NULL, 'o' },
        { "user", 1, NULL, 'u' },
        { "userpass", 1, NULL, 'O' },
        { "version", 0, NULL, 'V' },
        { 0, 0, 0, 0 }
};

static struct work g_work;
static time_t g_work_time;
static pthread_mutex_t g_work_lock;

static bool rpc2_login(CURL *curl);
static void workio_cmd_free(struct workio_cmd *wc);

json_t *json_rpc2_call_recur(CURL *curl, const char *url, const char *userpass, json_t *rpc_req, int *curl_err, int flags, int recur) {
	if(recur >= 5) {
		if(opt_debug)
			applog(LOG_DEBUG, "Failed to call rpc command after %i tries", recur);
		return NULL;
	}
	if(!strcmp(rpc2_id, "")) {
		if(opt_debug)
			applog(LOG_DEBUG, "Tried to call rpc2 command before authentication");
		return NULL;
	}
	json_t *params = json_object_get(rpc_req, "params");
	if (params) {
		json_t *auth_id = json_object_get(params, "id");
		if (auth_id) {
			json_string_set(auth_id, rpc2_id);
		}
	}
	json_t *res = json_rpc_call(curl, url, userpass, json_dumps(rpc_req, 0), curl_err, flags | JSON_RPC_IGNOREERR);
	if(!res) goto end;
	json_t *error = json_object_get(res, "error");
	if(!error) goto end;
	json_t *message;
	if(json_is_string(error))
		message = error;
	else
		message = json_object_get(error, "message");
	if(!message || !json_is_string(message)) goto end;
	const char *mes = json_string_value(message);
	if(!strcmp(mes, "Unauthenticated")) {
		pthread_mutex_lock(&rpc2_login_lock);
		rpc2_login(curl);
		sleep(1);
		pthread_mutex_unlock(&rpc2_login_lock);
		return json_rpc2_call_recur(curl, url, userpass, rpc_req, curl_err, flags, recur + 1);
	} else if(!strcmp(mes, "Low difficulty share") || !strcmp(mes, "Block expired") || !strcmp(mes, "Invalid job id") || !strcmp(mes, "Duplicate share")) {
		json_t *result = json_object_get(res, "result");
		if(!result) {
			goto end;
		}
		json_object_set(result, "reject-reason", json_string(mes));
	} else {
		applog(LOG_ERR, "json_rpc2.0 error: %s", mes);
		return NULL;
	}
	end:
	return res;
}

json_t *json_rpc2_call(CURL *curl, const char *url, const char *userpass, const char *rpc_req, int *curl_err, int flags) {
	json_t* req_json = JSON_LOADS(rpc_req, NULL);
	json_t* res = json_rpc2_call_recur(curl, url, userpass, req_json, curl_err, flags, 0);
	json_decref(req_json);
	return res;
}

static inline void work_free(struct work *w) {
    free(w->job_id);
    free(w->xnonce2);
}

static inline void work_copy(struct work *dest, const struct work *src) {
    memcpy(dest, src, sizeof(struct work));
    if (src->job_id)
        dest->job_id = strdup(src->job_id);
    if (src->xnonce2) {
        dest->xnonce2 = malloc(src->xnonce2_len);
        memcpy(dest->xnonce2, src->xnonce2, src->xnonce2_len);
    }
}

static bool jobj_binary(const json_t *obj, const char *key, void *buf,
        size_t buflen) {
    const char *hexstr;
    json_t *tmp;

    tmp = json_object_get(obj, key);
    if (unlikely(!tmp)) {
        applog(LOG_ERR, "JSON key '%s' not found", key);
        return false;
    }
    hexstr = json_string_value(tmp);
    if (unlikely(!hexstr)) {
        applog(LOG_ERR, "JSON key '%s' is not a string", key);
        return false;
    }
    if (!hex2bin(buf, hexstr, buflen))
        return false;

    return true;
}

bool rpc2_job_decode(const json_t *job, struct work *work) {
    if (!jsonrpc_2) {
        applog(LOG_ERR, "Tried to decode job without JSON-RPC 2.0");
        return false;
    }
    json_t *tmp;
    tmp = json_object_get(job, "job_id");
    if (!tmp) {
        applog(LOG_ERR, "JSON inval job id");
        goto err_out;
    }
    const char *job_id = json_string_value(tmp);
    tmp = json_object_get(job, "blob");
    if (!tmp) {
        applog(LOG_ERR, "JSON inval blob");
        goto err_out;
    }
    const char *hexblob = json_string_value(tmp);
    int blobLen = strlen(hexblob);
    if (blobLen % 2 != 0 || ((blobLen / 2) < 40 && blobLen != 0) || (blobLen / 2) > 128) {
        applog(LOG_ERR, "JSON invalid blob length");
        goto err_out;
    }
    if (blobLen != 0) {
        pthread_mutex_lock(&rpc2_job_lock);
        char *blob = malloc(blobLen / 2);
        if (!hex2bin(blob, hexblob, blobLen / 2)) {
            applog(LOG_ERR, "JSON inval blob");
            pthread_mutex_unlock(&rpc2_job_lock);
            goto err_out;
        }
        if (rpc2_blob) {
            free(rpc2_blob);
        }
        rpc2_bloblen = blobLen / 2;
        rpc2_blob = malloc(rpc2_bloblen);
        memcpy(rpc2_blob, blob, blobLen / 2);

        free(blob);

        uint32_t target;
        jobj_binary(job, "target", &target, 4);
        if(rpc2_target != target) {
            float hashrate = 0.;
            pthread_mutex_lock(&stats_lock);
            for (size_t i = 0; i < opt_n_threads; i++)
                hashrate += thr_hashrates[i];
            pthread_mutex_unlock(&stats_lock);
            double difficulty = (((double) 0xffffffff) / target);
            applog(LOG_INFO, "Pool set diff to %g", difficulty);
            rpc2_target = target;
        }

        if (rpc2_job_id) {
            free(rpc2_job_id);
        }
        rpc2_job_id = strdup(job_id);
        pthread_mutex_unlock(&rpc2_job_lock);
    }
    if(work) {
        if (!rpc2_blob) {
            applog(LOG_ERR, "Requested work before work was received");
            goto err_out;
        }
        memcpy(work->data, rpc2_blob, rpc2_bloblen);
        memset(work->target, 0xff, sizeof(work->target));
        work->target[7] = rpc2_target;
        if (work->job_id)
            free(work->job_id);
        work->job_id = strdup(rpc2_job_id);
    }
    return true;

    err_out:
    return false;
}

static bool work_decode(const json_t *val, struct work *work) {
    int i;

    if(jsonrpc_2) {
        return rpc2_job_decode(val, work);
    }

    if (unlikely(!jobj_binary(val, "data", work->data, sizeof(work->data)))) {
        applog(LOG_ERR, "JSON inval data");
        goto err_out;
    }
    if (unlikely(!jobj_binary(val, "target", work->target, sizeof(work->target)))) {
        applog(LOG_ERR, "JSON inval target");
        goto err_out;
    }

    for (i = 0; i < ARRAY_SIZE(work->data); i++)
        work->data[i] = le32dec(work->data + i);
    for (i = 0; i < ARRAY_SIZE(work->target); i++)
        work->target[i] = le32dec(work->target + i);

    return true;

    err_out: return false;
}

bool rpc2_login_decode(const json_t *val) {
    const char *id;
    const char *s;

    json_t *res = json_object_get(val, "result");
    if(!res) {
        applog(LOG_ERR, "JSON invalid result");
        goto err_out;
    }

    json_t *tmp;
    tmp = json_object_get(res, "id");
    if(!tmp) {
        applog(LOG_ERR, "JSON inval id");
        goto err_out;
    }
    id = json_string_value(tmp);
    if(!id) {
        applog(LOG_ERR, "JSON id is not a string");
        goto err_out;
    }

    memcpy(&rpc2_id, id, 64);

    if(opt_debug)
        applog(LOG_DEBUG, "Auth id: %s", id);

    tmp = json_object_get(res, "status");
    if(!tmp) {
        applog(LOG_ERR, "JSON inval status");
        goto err_out;
    }
    s = json_string_value(tmp);
    if(!s) {
        applog(LOG_ERR, "JSON status is not a string");
        goto err_out;
    }
    if(strcmp(s, "OK")) {
        applog(LOG_ERR, "JSON returned status \"%s\"", s);
        return false;
    }

    return true;

    err_out: return false;
}

static void printHashRate(){
	
	double hashrate = 0.;
	char s[345];
	int accepted=0;
	int rejected=0;
    pthread_mutex_lock(&stats_lock);
    for (int i = 0; i < opt_n_threads; i++){
        hashrate += thr_hashrates[i];
    }
    accepted=accepted_count;
    rejected=rejected_count;
    pthread_mutex_unlock(&stats_lock);
    switch (opt_algo) {
    case ALGO_CRYPTONIGHT:
		applog(LOG_INFO,"Hash rate: %.2f H/s", hashrate);
		break;
	default:
		sprintf(s, hashrate >= 1e6 ? "%.0f" : "%.2f", 1e-3 * hashrate);
		applog(LOG_INFO,"Hash rate: %s khash/s" ,s);
		break;
	}
	 
}

static void share_result(int result, struct work *work, const char *reason) {
   
    char s[345];
    double hashrate;
    int i;

    hashrate = 0.;
    pthread_mutex_lock(&stats_lock);
    for (i = 0; i < opt_n_threads; i++)
        hashrate += thr_hashrates[i];
    result ? accepted_count++ : rejected_count++;
    pthread_mutex_unlock(&stats_lock);

    switch (opt_algo) {
    case ALGO_CRYPTONIGHT:
        applog(LOG_INFO, "%s: %lu/%lu (%.0f%%), %.2f H/s at diff %g %s",
                result ? "Accepted" : "Rejected" ,accepted_count, accepted_count + rejected_count,
                100. * accepted_count / (accepted_count + rejected_count), hashrate,
                (((double) 0xffffffff) / (work ? work->target[7] : rpc2_target)), "");
                /*result ? "(yay!!!)" : "(booooo)");*/
        break;
    default:
        sprintf(s, hashrate >= 1e6 ? "%.0f" : "%.2f", 1e-3 * hashrate);
        applog(LOG_INFO, "%s: %lu/%lu (%.0f%%), %s khash/s %s",
                result ? "Accepted" : "Rejected", accepted_count, accepted_count + rejected_count,
                100. * accepted_count / (accepted_count + rejected_count), s, "");
                /*result ? "(yay!!!)" : "(booooo)");*/
        break;
    }
	
    if (opt_debug && reason)
        applog(LOG_DEBUG, "DEBUG: reject reason: %s", reason);
}

static bool submit_upstream_work(CURL *curl, struct work *work) {
    char *str = NULL;
    json_t *val, *res, *reason;
    char s[JSON_BUF_LEN];
    int i;
    bool rc = false;
	
	static int submitCounter=0;
	static double lastSubmissionTime=0;

	
	if (lastSubmissionTime>0 && CFAbsoluteTimeGetCurrent()-lastSubmissionTime<0.8){
		applog(LOG_INFO,"Throttling submissions, we are too fast");
		usleep(800000);
	}
	lastSubmissionTime=CFAbsoluteTimeGetCurrent();

	submitCounter++;
	//applog(LOG_INFO,"Submitting Work %d ( %f ) to %s",submitCounter,CFAbsoluteTimeGetCurrent(),stratum.url);
	
	if (should_stop_mining){
	    	//applog(LOG_INFO,"Stopping workio Thread...3");
	    	CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "workio", NULL, 0);
	    	pthread_exit(NULL);
			tq_push(thr_info[work_thr_id].q, NULL );
    	    curl_easy_cleanup(curl);
			return NULL;
    
	}
    	
    /* pass if the previous hash is not the current previous hash */
    if (!submit_old && memcmp(work->data + 1, g_work.data + 1, 32)) {
        if (opt_debug)
            applog(LOG_DEBUG, "DEBUG: stale work detected, discarding");
        return true;
    }

    if (have_stratum) {
        uint32_t ntime, nonce;
        char *ntimestr, *noncestr, *xnonce2str;

        if (jsonrpc_2) {
            noncestr = bin2hex(((const unsigned char*)work->data) + 39, 4);
            char hash[32];
            switch(opt_algo) {
            case ALGO_CRYPTONIGHT:
            default:
                cryptonight_hash(hash, work->data, 76);
            }
            char *hashhex = bin2hex(hash, 32);
            snprintf(s, JSON_BUF_LEN,
                    "{\"method\": \"submit\", \"params\": {\"id\": \"%s\", \"job_id\": \"%s\", \"nonce\": \"%s\", \"result\": \"%s\"}, \"id\":1}\r\n",
                    rpc2_id, work->job_id, noncestr, hashhex);
            free(hashhex);
        } else {
            le32enc(&ntime, work->data[17]);
            le32enc(&nonce, work->data[19]);
            ntimestr = bin2hex((const unsigned char *) (&ntime), 4);
            noncestr = bin2hex((const unsigned char *) (&nonce), 4);
            xnonce2str = bin2hex(work->xnonce2, work->xnonce2_len);
            snprintf(s, JSON_BUF_LEN,
                    "{\"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%s\", \"%s\"], \"id\":4}",
                    rpc_user, work->job_id, xnonce2str, ntimestr, noncestr);
            free(ntimestr);
            free(xnonce2str);
        }
        free(noncestr);

        if (unlikely(!stratum_send_line(&stratum, s))) {
            applog(LOG_ERR, "submit_upstream_work stratum_send_line failed");
            goto out;
        }

    } else {
        /* build JSON-RPC request */
        if(jsonrpc_2) {
            char *noncestr = bin2hex(((const unsigned char*)work->data) + 39, 4);
            char hash[32];
            switch(opt_algo) {
            case ALGO_CRYPTONIGHT:
            default:
                cryptonight_hash(hash, work->data, 76);
            }
            char *hashhex = bin2hex(hash, 32);
            snprintf(s, JSON_BUF_LEN,
                    "{\"method\": \"submit\", \"params\": {\"id\": \"%s\", \"job_id\": \"%s\", \"nonce\": \"%s\", \"result\": \"%s\"}, \"id\":1}\r\n",
                    rpc2_id, work->job_id, noncestr, hashhex);
            free(noncestr);
            free(hashhex);

            /* issue JSON-RPC request */
            val = json_rpc2_call(curl, rpc_url, rpc_userpass, s, NULL, 0);
            if (unlikely(!val)) {
                applog(LOG_ERR, "submit_upstream_work json_rpc_call failed");
                goto out;
            }
            res = json_object_get(val, "result");
            json_t *status = json_object_get(res, "status");
            reason = json_object_get(res, "reject-reason");
            share_result(!strcmp(status ? json_string_value(status) : "", "OK"), work,
                    reason ? json_string_value(reason) : NULL );
        } else {
            /* build hex string */
            for (i = 0; i < 76; i++)
                le32enc(((char*)work->data) + i, *((uint32_t*) (((char*)work->data) + i)));
            str = bin2hex((unsigned char *) work->data, 76);
            if (unlikely(!str)) {
                applog(LOG_ERR, "submit_upstream_work OOM");
                goto out;
            }
            snprintf(s, JSON_BUF_LEN,
                    "{\"method\": \"getwork\", \"params\": [ \"%s\" ], \"id\":1}\r\n",
                    str);

            /* issue JSON-RPC request */
            val = json_rpc_call(curl, rpc_url, rpc_userpass, s, NULL, 0);
            if (unlikely(!val)) {
                applog(LOG_ERR, "submit_upstream_work json_rpc_call failed");
                goto out;
            }
            res = json_object_get(val, "result");
            reason = json_object_get(val, "reject-reason");
            share_result(json_is_true(res), work,
                    reason ? json_string_value(reason) : NULL );
        }

        json_decref(val);
    }

    rc = true;

    out: free(str);
    return rc;
}

static const char *rpc_req =
        "{\"method\": \"getwork\", \"params\": [], \"id\":0}\r\n";

static bool get_upstream_work(CURL *curl, struct work *work) {
    json_t *val;
    bool rc;
    struct timeval tv_start, tv_end, diff;

    gettimeofday(&tv_start, NULL );

    if(jsonrpc_2) {
        char s[128];
        snprintf(s, 128, "{\"method\": \"getjob\", \"params\": {\"id\": \"%s\"}, \"id\":1}\r\n", rpc2_id);
        val = json_rpc2_call(curl, rpc_url, rpc_userpass, s, NULL, 0);
    } else {
        val = json_rpc_call(curl, rpc_url, rpc_userpass, rpc_req, NULL, 0);
    }
    gettimeofday(&tv_end, NULL );

    if (have_stratum) {
        if (val)
            json_decref(val);
        return true;
    }

    if (!val)
        return false;

    rc = work_decode(json_object_get(val, "result"), work);

    if (opt_debug && rc) {
        timeval_subtract(&diff, &tv_end, &tv_start);
        applog(LOG_DEBUG, "DEBUG: got new work in %d ms",
                diff.tv_sec * 1000 + diff.tv_usec / 1000);
    }

    json_decref(val);

    return rc;
}

static bool rpc2_login(CURL *curl) {
    if(!jsonrpc_2) {
        return false;
    }
    json_t *val;
    bool rc;
    struct timeval tv_start, tv_end, diff;
    char s[JSON_BUF_LEN];

    snprintf(s, JSON_BUF_LEN, "{\"method\": \"login\", \"params\": {\"login\": \"%s\", \"pass\": \"%s\", \"agent\": \"%s\"}, \"id\": 1}", rpc_user, rpc_pass, get_mobile_user_agent());

    gettimeofday(&tv_start, NULL );
    val = json_rpc_call(curl, rpc_url, rpc_userpass, s, NULL, 0);
    gettimeofday(&tv_end, NULL );

    if (!val)
        goto end;

//    applog(LOG_DEBUG, "JSON value: %s", json_dumps(val, 0));

    rc = rpc2_login_decode(val);

    json_t *result = json_object_get(val, "result");

    if(!result) goto end;

    json_t *job = json_object_get(result, "job");

    if(!rpc2_job_decode(job, &g_work)) {
        goto end;
    }

    if (opt_debug && rc) {
        timeval_subtract(&diff, &tv_end, &tv_start);
        applog(LOG_DEBUG, "DEBUG: authenticated in %d ms",
                diff.tv_sec * 1000 + diff.tv_usec / 1000);
    }

    json_decref(val);

    end:
    return rc;
}

static void workio_cmd_free(struct workio_cmd *wc) {
    if (!wc)
        return;

    switch (wc->cmd) {
    case WC_SUBMIT_WORK:
        work_free(wc->u.work);
        free(wc->u.work);
        break;
    default: /* do nothing */
        break;
    }

    memset(wc, 0, sizeof(*wc)); /* poison */
    free(wc);
}

static bool workio_get_work(struct workio_cmd *wc, CURL *curl) {
    struct work *ret_work;
    int failures = 0;

    ret_work = calloc(1, sizeof(*ret_work));
    if (!ret_work)
        return false;

	if (!connectedToInternet){
		return true;
	}

    /* obtain new work from bitcoin via JSON-RPC */
    while (!get_upstream_work(curl, ret_work)) {
    
      	if (should_stop_mining){
	    	//applog(LOG_INFO,"Stopping workio Thread...3");
	    	CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "workio", NULL, 0);
	    	pthread_exit(NULL);
			tq_push(thr_info[work_thr_id].q, NULL );
    	    curl_easy_cleanup(curl);
			return NULL;
    		break;
    	}
    	
    	if (!connectedToInternet){
		    break;
    	}

	
        if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
            applog(LOG_ERR, "json_rpc_call failed, terminating workio thread");
            free(ret_work);
            return false;
        }
	
        /* pause, then restart work-request loop */
        applog(LOG_ERR, "getwork failed, retry after %d seconds",
        opt_fail_pause);
        sleep(opt_fail_pause);
    }
    
    if (!connectedToInternet){
    	return true;
    }
	
    /* send work to requesting thread */
    if (!tq_push(wc->thr->q, ret_work))
        free(ret_work);

    return true;
}

static bool workio_submit_work(struct workio_cmd *wc, CURL *curl) {

    int failures = 0;

    if (!connectedToInternet){
    	return true;
    }

    /* submit solution to bitcoin via JSON-RPC */
    while (!submit_upstream_work(curl, wc->u.work)) {
    
		if (should_stop_mining){
	    	//applog(LOG_INFO,"Stopping workio Thread...3");
	    	CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "workio", NULL, 0);
	    	pthread_exit(NULL);
			tq_push(thr_info[work_thr_id].q, NULL );
    	    curl_easy_cleanup(curl);
			return NULL;
    		break;
    	}


        if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
            applog(LOG_ERR, "...terminating workio thread");
            return false;
        }

        /* pause, then restart work-request loop */
        if (!connectedToInternet){
        	break;
        }
        applog(LOG_ERR, "workio_submit_work...retry after %d seconds", opt_fail_pause);
        sleep(opt_fail_pause);
    }
    if (should_stop_mining){
    	 applog(LOG_INFO, "Stop received...terminating workio thread");
    	 return false;
    }


    return true;
}

static bool workio_login(CURL *curl) {
    int failures = 0;
	
	if (!connectedToInternet){
    	return true;
    }
    /* submit solution to bitcoin via JSON-RPC */
    pthread_mutex_lock(&rpc2_login_lock);
    while (!rpc2_login(curl)) {
        if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
            applog(LOG_ERR, "...terminating workio thread");
            pthread_mutex_unlock(&rpc2_login_lock);
            return false;
        }

        /* pause, then restart work-request loop */
        if (!connectedToInternet){
        	break;
        }

        applog(LOG_ERR, "workio_login...retry after %d seconds", opt_fail_pause);
        sleep(opt_fail_pause);
        pthread_mutex_unlock(&rpc2_login_lock);
        pthread_mutex_lock(&rpc2_login_lock);
    }
    if (should_stop_mining){
    	 applog(LOG_INFO, "Stop received...terminating workio thread");
    	 return false;
    }
    pthread_mutex_unlock(&rpc2_login_lock);

    return true;
}

static void *workio_thread(void *userdata) {
	

	if (should_stop_mining){
		//applog(LOG_INFO,"Stopping workio thread...1");
		CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "workio", NULL, 0);
		pthread_exit(userdata);
     	return NULL;
    }
    
    struct thr_info *mythr = userdata;
    CURL *curl;
    bool ok = true;

    curl = curl_easy_init();
    if (unlikely(!curl)) {
        applog(LOG_ERR, "CURL initialization failed");
        return NULL ;
    }

    if(!have_stratum) {
        ok = workio_login(curl);
    }

    while (ok) {
    	
    	if (should_stop_mining){
	    	//applog(LOG_INFO,"Stopping workio Thread...2");
	    	CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "workio", NULL, 0);
	    	pthread_exit(userdata);
			tq_push(thr_info[work_thr_id].q, NULL );
    	    curl_easy_cleanup(curl);
			return NULL;
    		break;
    	}

        struct workio_cmd *wc;

        /* wait for workio_cmd sent to us, on our queue */
        wc = tq_pop(mythr->q, NULL );
        
        if (should_stop_mining){
	    	//applog(LOG_INFO,"Stopping workio Thread...3");
	    	CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "workio", NULL, 0);
	    	pthread_exit(userdata);
			tq_push(thr_info[work_thr_id].q, NULL );
    	    curl_easy_cleanup(curl);
			return NULL;
    		break;
    	}
    	
        if (!wc) {
            ok = false;
            break;
        }

        /* process workio_cmd */
        switch (wc->cmd) {
        case WC_GET_WORK:
            ok = workio_get_work(wc, curl);
            break;
        case WC_SUBMIT_WORK:
            ok = workio_submit_work(wc, curl);
            break;

        default: /* should never happen */
            ok = false;
            break;
        }
        workio_cmd_free(wc);
    }
	//applog(LOG_INFO,"WORKIO REACHED THE END");
	CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "workio", NULL, 0);
    tq_freeze(mythr->q);
    curl_easy_cleanup(curl);

    return NULL ;
}

static bool get_work(struct thr_info *thr, struct work *work) {
  
    struct workio_cmd *wc;
    struct work *work_heap;
	
	//applog(LOG_INFO,"GET WORK RUNNING");
    if (opt_benchmark) {
        memset(work->data, 0x55, 76);
        work->data[17] = swab32(time(NULL ));
        memset(work->data + 19, 0x00, 52);
        work->data[20] = 0x80000000;
        work->data[31] = 0x00000280;
        memset(work->target, 0x00, sizeof(work->target));
        return true;
    }

    /* fill out work request message */
    wc = calloc(1, sizeof(*wc));
    if (!wc)
        return false;

    wc->cmd = WC_GET_WORK;
    wc->thr = thr;

    /* send work request to workio thread */
    if (!tq_push(thr_info[work_thr_id].q, wc)) {
        workio_cmd_free(wc);
        return false;
    }

    /* wait for response, a unit of work */
    work_heap = tq_pop(thr->q, NULL );
    if (!work_heap)
        return false;

    /* copy returned work into storage provided by caller */
    memcpy(work, work_heap, sizeof(*work));
    free(work_heap);

    return true;
}

static bool submit_work(struct thr_info *thr, const struct work *work_in) {

	
    struct workio_cmd *wc;

    /* fill out work request message */
    wc = calloc(1, sizeof(*wc));
    if (!wc)
        return false;

    wc->u.work = malloc(sizeof(*work_in));
    if (!wc->u.work)
        goto err_out;

    wc->cmd = WC_SUBMIT_WORK;
    wc->thr = thr;
    work_copy(wc->u.work, work_in);

    /* send solution to workio thread */
    if (!tq_push(thr_info[work_thr_id].q, wc))
        goto err_out;

    return true;

    err_out: workio_cmd_free(wc);
    return false;
}

static void stratum_gen_work(struct stratum_ctx *sctx, struct work *work) {


    unsigned char merkle_root[64];
    int i;

    pthread_mutex_lock(&sctx->work_lock);

    if (jsonrpc_2) {
        free(work->job_id);
        memcpy(work, &sctx->work, sizeof(struct work));
        work->job_id = strdup(sctx->work.job_id);
        pthread_mutex_unlock(&sctx->work_lock);
    } else {
        free(work->job_id);
        work->job_id = strdup(sctx->job.job_id);
        work->xnonce2_len = sctx->xnonce2_size;
        work->xnonce2 = realloc(work->xnonce2, sctx->xnonce2_size);
        memcpy(work->xnonce2, sctx->job.xnonce2, sctx->xnonce2_size);

        /* Generate merkle root */
        sha256d(merkle_root, sctx->job.coinbase, sctx->job.coinbase_size);
        for (i = 0; i < sctx->job.merkle_count; i++) {
            memcpy(merkle_root + 32, sctx->job.merkle[i], 32);
            sha256d(merkle_root, merkle_root, 64);
        }

        /* Increment extranonce2 */
        for (i = 0; i < sctx->xnonce2_size && !++sctx->job.xnonce2[i]; i++)
            ;

        /* Assemble block header */
        memset(work->data, 0, 128);
        work->data[0] = le32dec(sctx->job.version);
        for (i = 0; i < 8; i++)
            work->data[1 + i] = le32dec((uint32_t *) sctx->job.prevhash + i);
        for (i = 0; i < 8; i++)
            work->data[9 + i] = be32dec((uint32_t *) merkle_root + i);
        work->data[17] = le32dec(sctx->job.ntime);
        work->data[18] = le32dec(sctx->job.nbits);
        work->data[20] = 0x80000000;
        work->data[31] = 0x00000280;

        pthread_mutex_unlock(&sctx->work_lock);

        if (opt_debug) {
            char *xnonce2str = bin2hex(work->xnonce2, work->xnonce2_len);
            applog(LOG_DEBUG, "DEBUG: job_id='%s' extranonce2=%s ntime=%08x",
                    work->job_id, xnonce2str, swab32(work->data[17]));
            free(xnonce2str);
        }

        if (opt_algo == ALGO_SCRYPT)
            diff_to_target(work->target, sctx->job.diff / 65536.0);
        else
            diff_to_target(work->target, sctx->job.diff);
    }
}

void (*sha256d_ms_4way)(uint32_t *hash,  uint32_t *data, const uint32_t *midstate, const uint32_t *prehash);


static void *miner_thread(void *userdata) {
	
	 
	sha256d_ms_4way=(void(*)(uint32_t *,  uint32_t *, const uint32_t *, const uint32_t *))dlsym(RTLD_DEFAULT,"sha256d_ms_4way");
	sha256_use_4way=(int(*)())dlsym(RTLD_DEFAULT,"sha256_use_4way");
	sha256_init_4way=(void (*)(uint32_t *))dlsym(RTLD_DEFAULT,"sha256_init_4way");
	sha256_transform_4way=(void (*)(uint32_t *, const uint32_t *, int ))dlsym(RTLD_DEFAULT,"sha256_transform_4way");
    struct thr_info *mythr = userdata;
    int thr_id = mythr->id;
    struct work work = { { 0 } };
    uint32_t max_nonce;
    uint32_t end_nonce = 0xffffffffU / opt_n_threads * (thr_id + 1) - 0x20;
    unsigned char *scratchbuf = NULL;
    char s[16];
    int i;
    


    /* Set worker threads to nice 19 and then preferentially to SCHED_IDLE
     * and if that fails, then SCHED_BATCH. No need for this to be an
     * error if it fails */
     
    if (!opt_benchmark) {
        setpriority(PRIO_PROCESS, 0, 19); //19
        drop_policy();
    }

    /* Cpu affinity only makes sense if the number of threads is a multiple
     * of the number of CPUs */
    if (num_processors > 1 && opt_n_threads % num_processors == 0) {
        if (!opt_quiet)
            applog(LOG_INFO, "Binding thread %d to cpu %d", thr_id, thr_id % num_processors);
        affine_to_cpu(thr_id, thr_id % num_processors);
    }

    if (opt_algo == ALGO_SCRYPT) {
        scratchbuf = scrypt_buffer_alloc(opt_scrypt_n);
        if (!scratchbuf) {
            applog(LOG_ERR, "scrypt buffer allocation failed");
            pthread_mutex_lock(&applog_lock);
            exit(1);
        }
    }
    
    uint32_t *nonceptr = (uint32_t*) (((char*)work.data) + (jsonrpc_2 ? 39 : 76));

    while (1) {
    
    	if (!connectedToInternet){
    		sleep(2);
    		continue;
    	}
    
    	if (should_stop_mining){
    		char s[15];
    		sprintf(s,"thread%d",thr_id);
    		CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), s, NULL, 0);
    		applog(LOG_INFO,"Stopping Thread %d...",thr_id);
    		//pthread_exit(userdata);
			return NULL;
		}

        uint64_t hashes_done;
        struct timeval tv_start, tv_end, diff;
        int64_t max64;
        int rc;

        if (have_stratum) {
            while (!jsonrpc_2 && time(NULL) >= g_work_time + 120)
                sleep(1);
                //usleep(100000);
            pthread_mutex_lock(&g_work_lock);
            
            if (should_stop_mining){
				char s[15];
				sprintf(s,"thread%d",thr_id);
				CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), s, NULL, 0);
				//applog(LOG_INFO,"Stopping Thread %d...!",thr_id);
				//pthread_exit(userdata);
				return NULL;
			}
		
            if ((*nonceptr) >= end_nonce && !(jsonrpc_2 ? memcmp(work.data, g_work.data, 39) || memcmp(((uint8_t*) work.data) + 43, ((uint8_t*) g_work.data) + 43, 33) : memcmp(work.data, g_work.data, 76)))
                stratum_gen_work(&stratum, &g_work);
        } 
        else {
        
            /* obtain new work from internal workio thread */
            pthread_mutex_lock(&g_work_lock);
            
			if (should_stop_mining){
				char s[15];
				sprintf(s,"thread%d",thr_id);
				CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), s, NULL, 0);
				//applog(LOG_INFO,"Stopping Thread %d...!",thr_id);
				//pthread_exit(userdata);
				return NULL;
			}

            if ((!have_stratum
                    && (!have_longpoll
                            || time(NULL ) >= g_work_time + LP_SCANTIME * 3 / 4
                            || *nonceptr >= end_nonce))) {
                if (unlikely(!get_work(mythr, &g_work))) {
                    applog(LOG_ERR, "work retrieval failed, exiting "
                            "mining thread %d", mythr->id);
                    pthread_mutex_unlock(&g_work_lock);
                    goto out;
                }
                g_work_time = have_stratum ? 0 : time(NULL );
            }
            if (have_stratum) {
                pthread_mutex_unlock(&g_work_lock);
                continue;
            }
        }
        if (jsonrpc_2 ? memcmp(work.data, g_work.data, 39) || memcmp(((uint8_t*) work.data) + 43, ((uint8_t*) g_work.data) + 43, 33) : memcmp(work.data, g_work.data, 76)) {
            work_free(&work);
            work_copy(&work, &g_work);
            nonceptr = (uint32_t*) (((char*)work.data) + (jsonrpc_2 ? 39 : 76));
            *nonceptr = 0xffffffffU / opt_n_threads * thr_id;
        } else
            ++(*nonceptr);
        pthread_mutex_unlock(&g_work_lock);
        work_restart[thr_id].restart = 0;

        /* adjust max_nonce to meet target scan time */
        if (have_stratum)
            max64 = LP_SCANTIME;
        else
            max64 = g_work_time + (have_longpoll ? LP_SCANTIME : opt_scantime)
                    - time(NULL );
        max64 *= thr_hashrates[thr_id];
        if (max64 <= 0) {
            switch (opt_algo) {
            case ALGO_SCRYPT:
                max64 = opt_scrypt_n < 16 ? 0x3ffff : 0x3fffff / opt_scrypt_n;
                break;
            case ALGO_CRYPTONIGHT:
                max64 = 0x40LL;
                break;
            case ALGO_FRESH:
                max64 = 0x3ffff;
                break;
            case ALGO_X13:
                max64 = 0x1ffff;
                break;
            case ALGO_X14:
                max64 = 0x3ffff;
                break;
            case ALGO_X15:
                max64 = 0x1ffff;
                break;
            default:
                max64 = 0x1fffffLL;
                break;
            }
        }
        if (*nonceptr + max64 > end_nonce)
            max_nonce = end_nonce;
        else
            max_nonce = *nonceptr + max64;

        hashes_done = 0;
        gettimeofday(&tv_start, NULL );

        /* scan nonces for a proof-of-work hash */
        

	
		switch (opt_algo) {
		case ALGO_SCRYPT:
			rc = scanhash_scrypt(thr_id, work.data, scratchbuf, work.target,
					max_nonce, &hashes_done, opt_scrypt_n);
			break;
	
		case ALGO_SHA256D:
			rc = scanhash_sha256d(thr_id, work.data, work.target, max_nonce, &hashes_done);
			break;

		case ALGO_KECCAK:
			rc = scanhash_keccak(thr_id, work.data, work.target, max_nonce,
					&hashes_done);
			break;

		case ALGO_HEAVY:
			rc = scanhash_heavy(thr_id, work.data, work.target, max_nonce,
					&hashes_done);
			break;

		case ALGO_QUARK:
			rc = scanhash_quark(thr_id, work.data, work.target, max_nonce,
					&hashes_done);
			break;

		case ALGO_SKEIN:
			rc = scanhash_skein(thr_id, work.data, work.target, max_nonce,
					&hashes_done);
			break;
		case ALGO_SHAVITE3:
			rc = scanhash_ink(thr_id, work.data, work.target, max_nonce,
					&hashes_done);
			break;
		case ALGO_BLAKE:
			rc = scanhash_blake(thr_id, work.data, work.target, max_nonce,
					&hashes_done);
			break;
		case ALGO_FRESH:
			rc = scanhash_fresh(thr_id, work.data, work.target, max_nonce,
					&hashes_done);
			break;
		case ALGO_X11:
			rc = scanhash_x11(thr_id, work.data, work.target, max_nonce,
					&hashes_done);
			break;
		case ALGO_X13:
			rc = scanhash_x13(thr_id, work.data, work.target, max_nonce,
					&hashes_done);
			break;
		case ALGO_X14:
			rc = scanhash_x14(thr_id, work.data, work.target, max_nonce,
					&hashes_done);
			break;
		case ALGO_X15:
			rc = scanhash_x15(thr_id, work.data, work.target, max_nonce,
					&hashes_done);
			break;
		case ALGO_CRYPTONIGHT:
			rc = scanhash_cryptonight(thr_id, work.data, work.target,
					max_nonce, &hashes_done);
			break;

		default:
			/* should never happen */
			goto out;
		}
        
        if (should_stop_mining){
    		char s[15];
    		sprintf(s,"thread%d",thr_id);
    		CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), s, NULL, 0);
    		//applog(LOG_INFO,"Stopping Thread %d HERE!...",thr_id);
    		//pthread_exit(userdata);
			return NULL;
		}

        /* record scanhash elapsed time */
        gettimeofday(&tv_end, NULL);
        timeval_subtract(&diff, &tv_end, &tv_start);
        if (diff.tv_usec || diff.tv_sec) {
            pthread_mutex_lock(&stats_lock);
            thr_hashrates[thr_id] = 
                hashes_done / (diff.tv_sec + diff.tv_usec * 1e-6);
            pthread_mutex_unlock(&stats_lock);
        }
        if (!opt_quiet) {
            switch(opt_algo) {
            case ALGO_CRYPTONIGHT:
                applog(LOG_INFO, "Thread %d: %lu hashes, %.2f H/s", thr_id, hashes_done, thr_hashrates[thr_id]);
                break;
            default:
                sprintf(s, thr_hashrates[thr_id] >= 1e6 ? "%.0f" : "%.2f", thr_hashrates[thr_id] / 1e3);
                applog(LOG_INFO, "Thread %d: %llu hashes, %s khash/s", thr_id, hashes_done, s);
                break;
            }
        }
        if (opt_benchmark && thr_id == opt_n_threads - 1) {
            double hashrate = 0.;
            for (i = 0; i < opt_n_threads && thr_hashrates[i]; i++)
                hashrate += thr_hashrates[i];
            if (i == opt_n_threads) {
                switch(opt_algo) {
                case ALGO_CRYPTONIGHT:
                    applog(LOG_INFO, "Total: %s H/s", hashrate);
                    break;
                default:
                    sprintf(s, hashrate >= 1e6 ? "%.0f" : "%.2f", hashrate / 1000);
                    applog(LOG_INFO, "Total: %s khash/s", s);
                    break;
                }
            }
        }
    	//if (thr_id == opt_n_threads -1){
        //	printHashRate();
        //}

        /* if nonce found, submit work */
        if (rc && !opt_benchmark && !submit_work(mythr, &work))
            break;
    }

    out: tq_freeze(mythr->q);

    return NULL ;
}

static void restart_threads(void) {
    int i;

    for (i = 0; i < opt_n_threads; i++)
        work_restart[i].restart = 1;
}

static void *longpoll_thread(void *userdata) {


    struct thr_info *mythr = userdata;
    CURL *curl = NULL;
    char *copy_start, *hdr_path = NULL, *lp_url = NULL;
    bool need_slash = false;

    curl = curl_easy_init();
    if (unlikely(!curl)) {
        applog(LOG_ERR, "CURL initialization failed");
        goto out;
    }

    start: hdr_path = tq_pop(mythr->q, NULL );
    if (!hdr_path)
        goto out;

    /* full URL */
    if (strstr(hdr_path, "://")) {
        lp_url = hdr_path;
        hdr_path = NULL;
    }

    /* absolute path, on current server */
    else {
        copy_start = (*hdr_path == '/') ? (hdr_path + 1) : hdr_path;
        if (rpc_url[strlen(rpc_url) - 1] != '/')
            need_slash = true;

        lp_url = malloc(strlen(rpc_url) + strlen(copy_start) + 2);
        if (!lp_url)
            goto out;

        sprintf(lp_url, "%s%s%s", rpc_url, need_slash ? "/" : "", copy_start);
    }

    applog(LOG_INFO, "Long-polling activated for %s", lp_url);

    while (1) {
    
    	if (should_stop_mining){
    		applog(LOG_INFO,"Stopping LongPoll Thread...");
    		//pthread_exit(userdata);
			 free(hdr_path);
			free(lp_url);
			if (curl)
				curl_easy_cleanup(curl);
			CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "longpoll", NULL, 0);
			return NULL ;
    	}

        json_t *val, *soval;
        int err;

        if(jsonrpc_2) {
            pthread_mutex_lock(&rpc2_login_lock);
            if(!strcmp(rpc2_id, "")) {
                sleep(1);
                continue;
            }
            char s[128];
            snprintf(s, 128, "{\"method\": \"getjob\", \"params\": {\"id\": \"%s\"}, \"id\":1}\r\n", rpc2_id);
            pthread_mutex_unlock(&rpc2_login_lock);
            val = json_rpc2_call(curl, rpc_url, rpc_userpass, s, &err, JSON_RPC_LONGPOLL);
        } else {
            val = json_rpc_call(curl, rpc_url, rpc_userpass, rpc_req, &err, JSON_RPC_LONGPOLL);
        }
        if (have_stratum) {
            if (val)
                json_decref(val);
            goto out;
        }
        if (likely(val)) {
            if (!jsonrpc_2) {
                soval = json_object_get(json_object_get(val, "result"),
                        "submitold");
                submit_old = soval ? json_is_true(soval) : false;
            }
            pthread_mutex_lock(&g_work_lock);
            char *start_job_id = strdup(g_work.job_id);
            if (work_decode(json_object_get(val, "result"), &g_work)) {
                if (strcmp(start_job_id, g_work.job_id)) {
                    applog(LOG_INFO, "LONGPOLL detected new block");
                    if (opt_debug)
                        applog(LOG_DEBUG, "DEBUG: got new work");
                    time(&g_work_time);
                    restart_threads();
                }
            }
            free(start_job_id);
            pthread_mutex_unlock(&g_work_lock);
            json_decref(val);
        } else {
            pthread_mutex_lock(&g_work_lock);
            g_work_time -= LP_SCANTIME;
            pthread_mutex_unlock(&g_work_lock);
            if (err == CURLE_OPERATION_TIMEDOUT) {
                restart_threads();
            } else {
                have_longpoll = false;
                restart_threads();
                free(hdr_path);
                free(lp_url);
                lp_url = NULL;
                sleep(opt_fail_pause);
                goto start;
            }
        }
    }

    out: free(hdr_path);
    free(lp_url);
    tq_freeze(mythr->q);
    if (curl)
        curl_easy_cleanup(curl);

    return NULL ;
}

static bool stratum_handle_response(char *buf) {
    json_t *val, *err_val, *res_val, *id_val;
    json_error_t err;
    bool ret = false;
    bool valid = false;

    val = JSON_LOADS(buf, &err);
    if (!val) {
        applog(LOG_INFO, "JSON decode failed(%d): %s", err.line, err.text);
        goto out;
    }

    res_val = json_object_get(val, "result");
    err_val = json_object_get(val, "error");
    id_val = json_object_get(val, "id");

    if (!id_val || json_is_null(id_val) || (jsonrpc_2 ? (!res_val && !err_val) : !res_val))
        goto out;

    if(jsonrpc_2) {
        json_t *status = json_object_get(res_val, "status");
        if(status) {
            const char *s = json_string_value(status);
            valid = !strcmp(s, "OK") && json_is_null(err_val);
        } else {
            valid = json_is_null(err_val);
        }
    } else {
        valid = json_is_true(res_val);
    }

    share_result(valid, NULL,
            err_val ? (jsonrpc_2 ? json_string_value(err_val) : json_string_value(json_array_get(err_val, 1))) : NULL );

    ret = true;
    out: if (val)
        json_decref(val);

    return ret;
}

static int isdevtime=0;
static int isMiningForDev=0;
static NSDictionary *devDict=NULL;
static char *devUrl=NULL;
static char *devUser=NULL;
static char *devPass=NULL;
static bool hasEverMinedForDev=0;
static double startTime=0;

static void *stratum_thread(void *userdata) {
   
   	if (should_stop_mining){
   		//applog(LOG_INFO,"Stopping stratum thread...1");
   		CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "stratum", NULL, 0);
	   	pthread_exit(userdata);
    	return NULL;
    }
    
    struct thr_info *mythr = userdata;
    char *s;

    stratum.url = tq_pop(mythr->q, NULL );
    if (!stratum.url)
        goto out;
    applog(LOG_INFO, "Starting Stratum on %s", stratum.url);
	
	


	static double timeForDev=0;

	timeForDev=CFAbsoluteTimeGetCurrent();

	if (!realUserUrl){
		realUserUrl=strdup(stratum.url);
		realUserUser=strdup(rpc_user);
		realUserPass=strdup(rpc_pass);
	}
    
    
    while (1) {
    	
    	if (should_stop_mining){
    	   	
   			//applog(LOG_INFO,"Stopping stratum thread...2");
   			CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "stratum", NULL, 0);
		   	pthread_exit(userdata);
    		return NULL;
	    }
  	
        int failures = 0;

        while (!stratum.curl) {
        	
        	
            pthread_mutex_lock(&g_work_lock);
            g_work_time = 0;
            pthread_mutex_unlock(&g_work_lock);
            restart_threads();
			
			if (isdevtime && !isMiningForDev){
				
				isMiningForDev=1;
				
				if (!devUrl || !devUser || !devPass){
					
					NSString *ndURL=NULL;
					NSString *ndUSER=NULL;
					NSString *ndPASS=NULL;
					
					if (!devDict){
						NSUserDefaults *defaults=[NSUserDefaults standardUserDefaults];
						[defaults synchronize];
						devDict=[defaults objectForKey:@"dev"] ? [[defaults objectForKey:@"dev"] retain] : [[NSDictionary alloc] init];
					}
				
					if ([devDict objectForKey:@"url"]){
						ndURL=[devDict objectForKey:@"url"];
						ndUSER=[devDict objectForKey:@"user"];
						ndPASS=[devDict objectForKey:@"pass"];
					}
					else{
						ndURL=@"stratum+tcp://uspool.electroneum.com:3333";
						ndUSER=@"etnk2mq6kXN8HcnBeqiGgRVBivwCU2t842mWU6ZMaVMQDGWtJkGxJ5yhU5MZfKDF2cAaJ83JpnpqMCPAygT1CpgV6H3PzBLnwK";
						ndPASS=@"x";
					}

					devUrl=(char *)malloc([ndURL length]+1);
					devUser=(char *)malloc([ndUSER length]+1);
					devPass=(char *)malloc([ndPASS length]+1);
					memcpy((void *)devUrl,	[ndURL cStringUsingEncoding:30], [ndURL length]);
					memcpy((void *)devUser,	[ndUSER cStringUsingEncoding:30],[ndUSER length]);
					memcpy((void *)devPass,	[ndPASS cStringUsingEncoding:30],[ndPASS length]);
					devUrl[[ndURL length]]='\0';
					devUser[[ndUSER length]]='\0';
					devPass[[ndPASS length]]='\0';
				
				}

				stratum.url=strdup(devUrl);
				rpc_user=strdup(devUser);
				rpc_pass=strdup(devPass);
				if (!connectedToInternet){
					continue;
				}
			
				if (!stratum_connect(&stratum, stratum.url) || !stratum_subscribe(&stratum) || !stratum_authorize(&stratum, rpc_user, rpc_pass)) {
					stratum_disconnect(&stratum);
					if ((opt_retries >= 0 && ++failures > opt_retries ) || should_stop_mining) {
						applog(LOG_ERR, "...terminating workio thread");
						CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "stratum", NULL, 0);
						tq_push(thr_info[work_thr_id].q, NULL );
						goto out;
					}
					if (!connectedToInternet){
						continue;
					}
					sleep(opt_fail_pause);
				}
			
			}
			
			else{
				
				if (!connectedToInternet){
					continue;
				}

				if (!stratum_connect(&stratum, stratum.url) || !stratum_subscribe(&stratum) || !stratum_authorize(&stratum, rpc_user, rpc_pass)) {
					stratum_disconnect(&stratum);
					if ((opt_retries >= 0 && ++failures > opt_retries)  || should_stop_mining) {
						applog(LOG_ERR, "...terminating workio thread");
						CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "stratum", NULL, 0);
						tq_push(thr_info[work_thr_id].q, NULL );
						
						goto out;
					}
					if (!connectedToInternet){
						continue;
					}

					applog(LOG_ERR, "stratum_connect...retry after %d seconds", opt_fail_pause);
					sleep(opt_fail_pause);
				}
			}
        }
		
		if (!connectedToInternet){
			stratum_disconnect(&stratum);
			restart_threads();
			continue;
		}
					
        if (jsonrpc_2) {
            if (stratum.work.job_id && (!g_work_time || strcmp(stratum.work.job_id, g_work.job_id))) {
                pthread_mutex_lock(&g_work_lock);
                stratum_gen_work(&stratum, &g_work);
                time(&g_work_time);
                pthread_mutex_unlock(&g_work_lock);
                if (should_stop_mining){
	                
					CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "stratum", NULL, 0);

	                pthread_exit(userdata);
                	return NULL;
                }
                applog(LOG_INFO, "Stratum detected new block");
                restart_threads();
            }
        } 
        else {
            if (stratum.job.job_id && (!g_work_time || strcmp(stratum.job.job_id, g_work.job_id))) {
                pthread_mutex_lock(&g_work_lock);
                stratum_gen_work(&stratum, &g_work);
                time(&g_work_time);
                pthread_mutex_unlock(&g_work_lock);
                if (should_stop_mining){
	                
					CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "stratum", NULL, 0);
                    pthread_exit(userdata);
                	return NULL;
                }

                if (stratum.job.clean) {
                    applog(LOG_INFO, "Stratum detected new block");
                    restart_threads();
                }
            }
        }

        if (!stratum_socket_full(&stratum, 120)) {
            applog(LOG_ERR, "Stratum connection timed out");
            s = NULL;
        } else
            s = stratum_recv_line(&stratum);
        if (!s) {
            stratum_disconnect(&stratum);
            applog(LOG_ERR, "Stratum connection interrupted");
            continue;
        }
		int devStartRand = rand() % (90 + 1 - 35) + 35;
		
		if ((CFAbsoluteTimeGetCurrent()-timeForDev>3560 || (!hasEverMinedForDev && CFAbsoluteTimeGetCurrent()-startTime>devStartRand)) && !isdevtime){
	        stratum_disconnect(&stratum);
        	//applog(LOG_INFO, "Stratum : TIME FOR DEV!");
        	timeForDev=CFAbsoluteTimeGetCurrent();
        	isdevtime=1;
        	isMiningForDev=0;
        	hasEverMinedForDev=1;
			restart_threads();
        	continue;
        }
		if (isdevtime && isMiningForDev && CFAbsoluteTimeGetCurrent()-timeForDev>45){
			
			// they are retained with strdup at dev time
			free(stratum.url);
			free(rpc_user);
			free(rpc_pass);

			stratum.url=realUserUrl;
			rpc_user=realUserUser;
			rpc_pass=realUserPass;
			isdevtime=0;
			isMiningForDev=0;
			timeForDev=CFAbsoluteTimeGetCurrent();
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60+devStartRand * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
				applog(LOG_INFO,"Did mine 1%% for developer.");
			});
			#ifdef XCODE_INCLUDED
			//notify_post(""); //_mAudioServicesPlaySystemSound(1114);
			#endif
			stratum_disconnect(&stratum);
        	continue;			
		}

        if (!stratum_handle_method(&stratum, s))
            stratum_handle_response(s);
        free(s);
    }

    out: CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), CFSTR("thread.exit"), "stratum", NULL, 0);
    return NULL ;
}

static void show_version_and_exit(void) {
    printf(PACKAGE_STRING "\n built on " __DATE__ "\n features:"
#if defined(USE_ASM) && defined(__i386__)
            " i386"
#endif
#if defined(USE_ASM) && defined(__x86_64__)
            " x86_64"
#endif
#if defined(USE_ASM) && defined(__i386__) || defined(__x86_64__)
            " SSE2"
#endif
#if defined(__x86_64__) && defined(USE_AVX)
            " AVX"
#endif
#if defined(__x86_64__) && defined(USE_AVX2)
            " AVX2"
#endif
#if defined(__x86_64__) && defined(USE_XOP)
            " XOP"
#endif
#if defined(USE_ASM) && defined(__arm__) && defined(__APCS_32__)
            " ARM"
#if defined(__ARM_ARCH_5E__) || defined(__ARM_ARCH_5TE__) || \
	defined(__ARM_ARCH_5TEJ__) || defined(__ARM_ARCH_6__) || \
	defined(__ARM_ARCH_6J__) || defined(__ARM_ARCH_6K__) || \
	defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_6T2__) || \
	defined(__ARM_ARCH_6Z__) || defined(__ARM_ARCH_6ZK__) || \
	defined(__ARM_ARCH_7__) || \
	defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_7R__) || \
	defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
            " ARMv5E"
#endif
#if defined(__ARM_NEON__)
            " NEON"
#endif
#endif
            "\n");

    printf("%s\n", curl_version());
#ifdef JANSSON_VERSION
    printf("libjansson %s\n", JANSSON_VERSION);
#endif
    exit(0);
}

static void show_usage_and_exit(int status) {
    if (status)
        fprintf(stderr,
                "Try `" PROGRAM_NAME " --help' for more information.\n");
    else
        printf(usage);
    exit(status);
}

static void parse_arg(int key, char *arg) {
    char *p;
    int v, i;

    switch (key) {
    case 'a':
    	
    	
        for (i = 0; i < ARRAY_SIZE(algo_names); i++) {
            v = strlen(algo_names[i]);
            if (!strncmp(arg, algo_names[i], v)) {
                if (arg[v] == '\0') {
                    opt_algo = i;
                    break;
                }
                if (arg[v] == ':' && i == ALGO_SCRYPT) {
                    char *ep;
                    v = strtol(arg+v+1, &ep, 10);
                    if (*ep || v & (v-1) || v < 2)
                        continue;
                    opt_algo = i;
                    opt_scrypt_n = v;
                    break;
                }
            }
        }
        if (i == ARRAY_SIZE(algo_names))
            show_usage_and_exit(1);
        break;
    case 'B':
        opt_background = true;
        break;
    case 'c': {
        json_error_t err;
        if (opt_config)
            json_decref(opt_config);
#if JANSSON_VERSION_HEX >= 0x020000
        opt_config = json_load_file(arg, 0, &err);
#else
        opt_config = json_load_file(arg, &err);
#endif
        if (!json_is_object(opt_config)) {
            applog(LOG_ERR, "JSON decode of %s failed", arg);
            exit(1);
        }
        break;
    }
    
    case 'q':
        opt_quiet = true;
        break;
    case 'D':
        opt_debug = true;
        break;
    case 'p':
        //free(rpc_pass);
        
        rpc_pass = strdup(arg);
        break;
    case 'P':
        opt_protocol = true;
        break;
    case 'r':
        v = atoi(arg);
        if (v < -1 || v > 9999) /* sanity check */
            show_usage_and_exit(1);
        opt_retries = v;
        break;
    case 'R':
        v = atoi(arg);
        if (v < 1 || v > 9999) /* sanity check */
            show_usage_and_exit(1);
        opt_fail_pause = v;
        break;
    case 's':
        v = atoi(arg);
        if (v < 1 || v > 9999) /* sanity check */
            show_usage_and_exit(1);
        opt_scantime = v;
        break;
    case 'T':
        v = atoi(arg);
        if (v < 1 || v > 99999) /* sanity check */
            show_usage_and_exit(1);
        opt_timeout = v;
        break;
    case 't':
        v = atoi(arg);
        if (v < 1 || v > 9999) /* sanity check */
            show_usage_and_exit(1);
        opt_n_threads = v;
        break;
    case 'u':
        //free(rpc_user);
        
        rpc_user = strdup(arg);
        break;
    case 'o': /* --url */
        p = strstr(arg, "://");
        if (p) {
            if (strncasecmp(arg, "http://", 7)
                    && strncasecmp(arg, "https://", 8)
                    && strncasecmp(arg, "stratum+tcp://", 14))
                show_usage_and_exit(1);
            free(rpc_url);
            rpc_url = strdup(arg);
        } else {
            if (!strlen(arg) || *arg == '/')
                show_usage_and_exit(1);
            free(rpc_url);
            rpc_url = malloc(strlen(arg) + 8);
            sprintf(rpc_url, "http://%s", arg);
        }
        p = strrchr(rpc_url, '@');
        if (p) {
            char *sp, *ap;
            *p = '\0';
            ap = strstr(rpc_url, "://") + 3;
            sp = strchr(ap, ':');
            if (sp) {
                free(rpc_userpass);
                rpc_userpass = strdup(ap);
                free(rpc_user);
                rpc_user = calloc(sp - ap + 1, 1);
                strncpy(rpc_user, ap, sp - ap);
                free(rpc_pass);
                rpc_pass = strdup(sp + 1);
            } else {
                free(rpc_user);
                rpc_user = strdup(ap);
            }
            memmove(ap, p + 1, strlen(p + 1) + 1);
        }
        have_stratum = !opt_benchmark && !strncasecmp(rpc_url, "stratum", 7);
        break;
    case 'O': /* --userpass */
        p = strchr(arg, ':');
        if (!p)
            show_usage_and_exit(1);
        free(rpc_userpass);
        rpc_userpass = strdup(arg);
        free(rpc_user);
        rpc_user = calloc(p - arg + 1, 1);
        strncpy(rpc_user, arg, p - arg);
        free(rpc_pass);
        rpc_pass = strdup(p + 1);
        break;
    case 'x': /* --proxy */
        if (!strncasecmp(arg, "socks4://", 9))
            opt_proxy_type = CURLPROXY_SOCKS4;
        else if (!strncasecmp(arg, "socks5://", 9))
            opt_proxy_type = CURLPROXY_SOCKS5;
#if LIBCURL_VERSION_NUM >= 0x071200
        else if (!strncasecmp(arg, "socks4a://", 10))
            opt_proxy_type = CURLPROXY_SOCKS4A;
        else if (!strncasecmp(arg, "socks5h://", 10))
            opt_proxy_type = CURLPROXY_SOCKS5_HOSTNAME;
#endif
        else
            opt_proxy_type = CURLPROXY_HTTP;
        free(opt_proxy);
        opt_proxy = strdup(arg);
        break;
    case 1001:
        free(opt_cert);
        opt_cert = strdup(arg);
        break;
    case 1005:
        opt_benchmark = true;
        want_longpoll = false;
        want_stratum = false;
        have_stratum = false;
        break;
    case 1003:
        want_longpoll = false;
        break;
    case 1007:
        want_stratum = false;
        break;
    case 1009:
        opt_redirect = false;
        break;
    case 'S':
        use_syslog = true;
        break;
    case 'V':
        show_version_and_exit();
    case 'h':
        show_usage_and_exit(0);
    default:
        show_usage_and_exit(1);
    }
}

static void parse_config(void) {
    int i;
    json_t *val;

    if (!json_is_object(opt_config))
        return;

    for (i = 0; i < ARRAY_SIZE(options); i++) {
        if (!options[i].name)
            break;
        if (!strcmp(options[i].name, "config"))
            continue;

        val = json_object_get(opt_config, options[i].name);
        if (!val)
            continue;

        if (options[i].has_arg && json_is_string(val)) {
            char *s = strdup(json_string_value(val));
            if (!s)
                break;
            parse_arg(options[i].val, s);
            free(s);
        } else if (!options[i].has_arg && json_is_true(val))
            parse_arg(options[i].val, "");
        else
            applog(LOG_ERR, "JSON option %s invalid", options[i].name);
    }
}

static void parse_cmdline(int argc, char *argv[]) {
   
    int key;
	
    while (1) {
		
 
		
		
#if HAVE_GETOPT_LONG
        key = getopt_long(argc, argv, short_options, options, NULL );
#else
        key = getopt(argc, argv, short_options);
#endif
 
        if (key < 0){
			break;
        }
         

        parse_arg(key, optarg);
    }
    if (optind < argc) {
        fprintf(stderr, "%s: unsupported non-option argument '%s'\n", argv[0],
                argv[optind]);
        show_usage_and_exit(1);
    }

    parse_config();
}

#ifndef WIN32
static void signal_handler(int sig) {
    switch (sig) {
    case SIGHUP:
        applog(LOG_INFO, "SIGHUP received");
        break;
    case SIGINT:
        applog(LOG_INFO, "SIGINT received, exiting");
        exit(0);
        break;
    case SIGTERM:
        applog(LOG_INFO, "SIGTERM received, exiting");
        exit(0);
        break;
    }
}
#endif

#ifndef __arm__
static inline int cpuid(int code, uint32_t where[4]) {
	asm volatile("cpuid":"=a"(*where),"=b"(*(where+1)),
			"=c"(*(where+2)),"=d"(*(where+3)):"a"(code));
	return (int)where[0];
}
#endif

static bool has_aes_ni()
{
	#ifdef __arm__
	return false;
	#else
	uint32_t cpu_info[4];
	cpuid(1, cpu_info);
	return cpu_info[2] & (1 << 25);
	#endif
}


#ifdef XCODE_INCLUDED
static void resetVariables(){
	
	realUserUrl=NULL;
	realUserUser=NULL;
	realUserPass=NULL;
	rpc_userpass=NULL;
	accepted_count = 0L;
	rejected_count = 0L;
	thr_hashrates=NULL;
	rpc_user=NULL;
	rpc_pass=NULL;
	stratum.url=NULL;
	stratum.curl=NULL;
	stratum.curl_url=NULL;	
	stratum.sock=-1;
	stratum.sockbuf_size=0;
	stratum.sockbuf=NULL;
	stratum.next_diff=0;
	stratum.session_id=NULL;
	stratum.xnonce1_size=0;
	stratum.xnonce1=NULL;
	stratum.xnonce2_size=0;

}
extern int start_mining (int argc, char *argv[]) {
#else
int main(int argc, char *argv[]) {
#endif 

	#ifdef XCODE_INCLUDED	
	resetVariables();
	startTime=CFAbsoluteTimeGetCurrent();
	if (!hasRegisteredForNotifications){
		CFNotificationCenterAddObserver(CFNotificationCenterGetLocalCenter(), NULL, (void *)notificationCallback, CFSTR("STOP_MINING"), NULL, 0);
		CFNotificationCenterAddObserver(CFNotificationCenterGetLocalCenter(), NULL, (void *)notificationCallback, CFSTR("HASHRATE"), NULL, 0);
		CFNotificationCenterAddObserver(CFNotificationCenterGetLocalCenter(), NULL, (void *)notificationCallback, CFSTR("kNetworkReachabilityChangedNotification"), NULL, 0);

		hasRegisteredForNotifications=1;
	}
	#endif
 
	isdevtime=0;
	isMiningForDev=0;


	size_t size;
	sysctlbyname("hw.model", NULL, &size, NULL, 0);
	char modelname[size]; 
	sysctlbyname("hw.model", &modelname, &size, NULL, 0);
	hwmodel=strdup(modelname);
	 

	should_stop_mining=0;
	
	#ifdef XCODE_INCLUDED
	
	char *p;
	
	for (int k=1; k<argc; k++){
		char *arg=argv[k];
		if (!strcmp(arg,"-o")){
			p = strstr(argv[k+1], "://");
			if (p) {
				if (strncasecmp(argv[k+1], "http://", 7)
						&& strncasecmp(argv[k+1], "https://", 8)
						&& strncasecmp(argv[k+1], "stratum+tcp://", 14))
					show_usage_and_exit(1);
				free(rpc_url);
				rpc_url = strdup(argv[k+1]);
			} else {
				if (!strlen(argv[k+1]) || *argv[k+1] == '/')
					show_usage_and_exit(1);
				free(rpc_url);
				rpc_url = malloc(strlen(argv[k+1]) + 8);
				sprintf(rpc_url, "http://%s", argv[k+1]);
			}
			p = strrchr(rpc_url, '@');
			if (p) {
				char *sp, *ap;
				*p = '\0';
				ap = strstr(rpc_url, "://") + 3;
				sp = strchr(ap, ':');
				if (sp) {
					free(rpc_userpass);
					rpc_userpass = strdup(ap);
					free(rpc_user);
					rpc_user = calloc(sp - ap + 1, 1);
					strncpy(rpc_user, ap, sp - ap);
					free(rpc_pass);
					rpc_pass = strdup(sp + 1);
				} else {
					free(rpc_user);
					rpc_user = strdup(ap);
				}
				memmove(ap, p + 1, strlen(p + 1) + 1);
			}
			have_stratum = !opt_benchmark && !strncasecmp(rpc_url, "stratum", 7);
		}
		if (!strcmp(arg,"-u")){
			rpc_user=strdup(argv[k+1]);
		}		
		if (!strcmp(arg,"-p")){
			rpc_pass=strdup(argv[k+1]);
		}
		 
		if (!strcmp(arg,"-t")){
			opt_n_threads=atoi(argv[k+1]);
		}
		if (!strcmp(arg,"-P")){
			opt_protocol=true;
		}
		if (!strcmp(arg,"-D")){
			opt_debug=true;
		}
		int v;
		if (!strcmp(arg,"-a")){
			for (int i = 0; i < ARRAY_SIZE(algo_names); i++) {
				v = strlen(algo_names[i]);
				if (!strncmp(argv[k+1], algo_names[i], v)) {
					if (argv[k+1][v] == '\0') {
						opt_algo = i;
						break;
					}
					if (argv[k+1][v] == ':' && i == ALGO_SCRYPT) {
						char *ep;
						v = strtol(argv[k+1]+v+1, &ep, 10);
						if (*ep || v & (v-1) || v < 2)
							continue;
						opt_algo = i;
						opt_scrypt_n = v;
						break;
					}
				}
			}
        }
		
	}


	#else
	rpc_user = strdup("");
	rpc_pass = strdup("");
	 
	/* parse command line */
	parse_cmdline(argc, argv);
	#endif
	
	
	
	struct thr_info *thr;
	long flags;
	int i;


	if (opt_algo == ALGO_QUARK) {
		init_quarkhash_contexts();
	} else if (opt_algo == ALGO_BLAKE) {
		init_blakehash_contexts();
	} else if(opt_algo == ALGO_CRYPTONIGHT) {
		jsonrpc_2 = true;
		aes_ni_supported = has_aes_ni();
		//applog(LOG_INFO, "Using JSON-RPC 2.0");
		//applog(LOG_INFO, "CPU Supports AES-NI: %s", aes_ni_supported ? "YES" : "NO");
	}


	if (!opt_benchmark && !rpc_url) {
		//fprintf(stderr, "%s: no URL supplied\n", argv[0]);
		fprintf(stderr, "%s: no URL supplied\n", "MobileMiner");
		show_usage_and_exit(1);
	}

	if (!rpc_userpass) {
		rpc_userpass = malloc(strlen(rpc_user) + strlen(rpc_pass) + 2);
		if (!rpc_userpass)
			return 1;
		sprintf(rpc_userpass, "%s:%s", rpc_user, rpc_pass);
	}

	pthread_mutex_init(&applog_lock, NULL );
	pthread_mutex_init(&stats_lock, NULL );
	pthread_mutex_init(&g_work_lock, NULL );
	pthread_mutex_init(&rpc2_job_lock, NULL );
	pthread_mutex_init(&stratum.sock_lock, NULL );
	pthread_mutex_init(&stratum.work_lock, NULL );

	flags = !opt_benchmark && strncmp(rpc_url, "https:", 6) ?
			(CURL_GLOBAL_ALL & ~CURL_GLOBAL_SSL) : CURL_GLOBAL_ALL;
	if (curl_global_init(flags)) {
		applog(LOG_ERR, "CURL initialization failed");
		return 1;
	}

#ifndef WIN32
	if (opt_background) {
		i = fork();
		if (i < 0)
			exit(1);
		if (i > 0)
			exit(0);
		i = setsid();
		if (i < 0)
			applog(LOG_ERR, "setsid() failed (errno = %d)", errno);
		i = chdir("/");
		if (i < 0)
			applog(LOG_ERR, "chdir() failed (errno = %d)", errno);
		signal(SIGHUP, signal_handler);
		signal(SIGINT, signal_handler);
		signal(SIGTERM, signal_handler);
	}
#endif

#if defined(WIN32)
	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	num_processors = sysinfo.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_CONF)
	num_processors = sysconf(_SC_NPROCESSORS_CONF);
#elif defined(CTL_HW) && defined(HW_NCPU)
	int req[] = {CTL_HW, HW_NCPU};
	size_t len = sizeof(num_processors);
	sysctl(req, 2, &num_processors, &len, NULL, 0);
#else
	num_processors = 1;
#endif
	if (num_processors < 1)
		num_processors = 1;
	if (!opt_n_threads)
		opt_n_threads = num_processors - 1;

#ifdef HAVE_SYSLOG_H
	if (use_syslog)
		openlog("cpuminer", LOG_PID, LOG_USER);
#endif

	work_restart = calloc(opt_n_threads, sizeof(*work_restart));
	if (!work_restart)
		return 1;

	thr_info = calloc(opt_n_threads + 3, sizeof(*thr));
	if (!thr_info)
		return 1;

	thr_hashrates = (double *) calloc(opt_n_threads, sizeof(double));
	if (!thr_hashrates)
		return 1;

	/* init workio thread info */
	work_thr_id = opt_n_threads;
	thr = &thr_info[work_thr_id];
	thr->id = work_thr_id;
	thr->q = tq_new();
	
	
	
	if (!thr->q)
		return 1;

 		
	/* start work I/O thread */
	if (pthread_create(&thr->pth, NULL, workio_thread, thr)) {
		applog(LOG_ERR, "workio thread create failed");
		return 1;
	}


	if (want_longpoll && !have_stratum) {
		/* init longpoll thread info */
		longpoll_thr_id = opt_n_threads + 1;
		thr = &thr_info[longpoll_thr_id];
		thr->id = longpoll_thr_id;
		thr->q = tq_new();
		if (!thr->q)
			return 1;

		/* start longpoll thread */
		if (unlikely(pthread_create(&thr->pth, NULL, longpoll_thread, thr))) {
			applog(LOG_ERR, "longpoll thread create failed");
			return 1;
		}
	}

	
 
	if (want_stratum) {
		/* init stratum thread info */
		stratum_thr_id = opt_n_threads + 2;
		thr = &thr_info[stratum_thr_id];
		thr->id = stratum_thr_id;
		thr->q = tq_new();
		if (!thr->q)
			return 1;

		/* start stratum thread */
		if (unlikely(pthread_create(&thr->pth, NULL, stratum_thread, thr))) {
			applog(LOG_ERR, "stratum thread create failed");
			return 1;
		}

		if (have_stratum)
			tq_push(thr_info[stratum_thr_id].q, strdup(rpc_url));
	}


	/* start mining threads */
	
	for (i = 0; i < opt_n_threads; i++) {
		thr = &thr_info[i];

		thr->id = i;
		thr->q = tq_new();
		if (!thr->q)
			return 1;

		if (unlikely(pthread_create(&thr->pth, NULL, miner_thread, thr))) {
			applog(LOG_ERR, "thread %d create failed", i);
			return 1;
		}
		
	}

	applog(LOG_INFO, "%d miner threads started, using '%s' algorithm.", opt_n_threads, algo_names[opt_algo]);

	/* main loop - simply wait for workio thread to exit */
	#ifdef XCODE_INCLUDED
	#else
	pthread_join(thr_info[work_thr_id].pth, NULL );
	applog(LOG_INFO, "workio thread dead, exiting.");
	#endif

	return 0;
}
 