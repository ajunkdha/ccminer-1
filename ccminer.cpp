﻿/*
 * Copyright 2010 Jeff Garzik
 * Copyright 2012-2014 pooler
 * Copyright 2014-2015 tpruvot
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include <ccminer-config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>

#include <curl/curl.h>
#include <openssl/sha.h>

#ifdef WIN32
#include <windows.h>
#include <stdint.h>
#include <process.h>
#else
#include <errno.h>
#include <sys/resource.h>
#if HAVE_SYS_SYSCTL_H
#include <sys/types.h>
#if HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <sys/sysctl.h>
#endif
#endif

#include "miner.h"
#include "algos.h"
#include "sia/sia-rpc.h"
#include <cuda_runtime.h>

#ifdef WIN32
#include <Mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include "compat/winansi.h"
BOOL WINAPI ConsoleHandler(DWORD);
#endif

#define PROGRAM_NAME		"ccminer"
#define LP_SCANTIME		20
#define HEAVYCOIN_BLKHDR_SZ		84
#define MNR_BLKHDR_SZ 80

#include "nvml.h"
#ifdef USE_WRAPNVML
nvml_handle *hnvml = NULL;
#endif

enum workio_commands {
	WC_GET_WORK,
	WC_SUBMIT_WORK,
	WC_ABORT,
};

struct workio_cmd {
	enum workio_commands	cmd;
	struct thr_info		*thr;
	union {
		struct mtp *mtp;
	} t;
	union {
		struct work	*work;
	} u;
	int pooln;
};

bool opt_debug = false;
bool opt_debug_diff = false;
bool opt_debug_threads = false;
bool opt_protocol = false;
bool opt_benchmark = false;
bool opt_showdiff = true;

// todo: limit use of these flags,
// prefer the pools[] attributes
bool want_longpoll = true;
bool have_longpoll = false;
bool want_stratum = true;
bool have_stratum = false;
bool allow_gbt = true;
bool allow_mininginfo = true;
bool check_dups = false;
bool check_stratum_jobs = false;

bool have_gbt = true;
bool allow_getwork = false;

static bool submit_old = false;
static double last_nonz_diff = 0;
bool use_syslog = false;
bool use_colors = true;
int use_pok = 0;
static bool opt_background = false;
bool opt_quiet = false;
int opt_maxlograte = 3;
static int opt_retries = -1;
static int opt_fail_pause = 2;
int opt_time_limit = -1;
int opt_shares_limit = -1;
time_t firstwork_time = 0;
int opt_timeout = 300; // curl
int opt_scantime = 30;
static json_t *opt_config;
static const bool opt_time = true;
volatile enum sha_algos opt_algo = ALGO_AUTO;
int opt_n_threads = 0;
int gpu_threads = 1;
//int64_t opt_affinity = 4096;
int64_t opt_affinity = -1;
int opt_priority = 0;
static double opt_difficulty = 1.;
bool opt_extranonce = true;
bool opt_trust_pool = false;
uint16_t opt_vote = 9999;
int num_cpus;
int active_gpus;
char * device_name[MAX_GPUS];
short device_map[MAX_GPUS] = { 0 };
uint32_t  device_sm[MAX_GPUS] = { 0 };
uint32_t gpus_intensity[MAX_GPUS] = { 0 };
uint32_t device_gpu_clocks[MAX_GPUS] = { 0 };
uint32_t device_mem_clocks[MAX_GPUS] = { 0 };
uint32_t device_plimit[MAX_GPUS] = { 0 };
uint8_t device_tlimit[MAX_GPUS] = { 0 };
int8_t device_pstate[MAX_GPUS] = { -1, -1 };
int32_t device_led[MAX_GPUS] = { -1, -1 };
int opt_led_mode = 0;
int opt_cudaschedule = -1;
static bool opt_keep_clocks = false;

// un-linked to cmdline scrypt options (useless)
int device_batchsize[MAX_GPUS] = { 0 };
int device_texturecache[MAX_GPUS] = { 0 };
int device_singlememory[MAX_GPUS] = { 0 };
// implemented scrypt options
int parallel = 2; // All should be made on GPU
char *device_config[MAX_GPUS] = { 0 };
int device_backoff[MAX_GPUS] = { 0 };
int device_lookup_gap[MAX_GPUS] = { 0 };
int device_interactive[MAX_GPUS] = { 0 };
int opt_nfactor = 0;
bool opt_autotune = true;
char *jane_params = NULL;

// pools (failover/getwork infos)
struct pool_infos pools[MAX_POOLS] = { 0 };
int num_pools = 1;
volatile int cur_pooln = 0;
bool opt_pool_failover = false;
volatile bool pool_on_hold = false;
volatile bool pool_is_switching = false;
volatile int pool_switch_count = 0;
bool conditional_pool_rotate = false;

// current connection
char *rpc_user = NULL;
char *rpc_pass;
char *rpc_url;
char *short_url = NULL;
static unsigned char pk_script[25] = { 0 };
static size_t pk_script_size = 0;
static char coinbase_sig[101] = { 0 };
struct stratum_ctx stratum = { 0 };
pthread_mutex_t stratum_sock_lock;
pthread_mutex_t stratum_work_lock;

char *opt_cert;
char *opt_proxy;
long opt_proxy_type;
struct thr_info *thr_info = NULL;
static int work_thr_id;
struct thr_api *thr_api;
int longpoll_thr_id = -1;
int stratum_thr_id = -1;
int api_thr_id = -1;
bool stratum_need_reset = false;
volatile bool abort_flag = false;
struct work_restart *work_restart = NULL;
static int app_exit_code = EXIT_CODE_OK;

pthread_mutex_t applog_lock;
pthread_mutex_t stats_lock;
double thr_hashrates[MAX_GPUS] = { 0 };
uint64_t global_hashrate = 0;
double   stratum_diff = 0.0;
double   net_diff = 0;
uint64_t net_hashrate = 0;
uint64_t net_blocks = 0;
// conditional mining
uint8_t conditional_state[MAX_GPUS] = { 0 };
double opt_max_temp = 0.0;
double opt_max_diff = -1.;
double opt_max_rate = -1.;
double opt_resume_temp = 0.;
double opt_donation = 0.25; // 0.25% donation (ie 1 share over 400 for dev) default value.
double opt_resume_diff = 0.;
double opt_resume_rate = -1.;

int opt_statsavg = 30;

// strdup on char* to allow a common free() if used
static char* opt_syslog_pfx = strdup(PROGRAM_NAME);
char *opt_api_allow = strdup("127.0.0.1"); /* 0.0.0.0 for all ips */
int opt_api_remote = 0;
int opt_api_listen = 4068; /* 0 to disable */

bool opt_stratum_stats = false;

static char const usage[] = "\
Usage: " PROGRAM_NAME " [OPTIONS]\n\
Options:\n\
  -a, --algo=ALGO       specify the hash algorithm to use\n\
			blake       Blake 256 (SFR)\n\
			blake2s     Blake2-S 256 (NEVA)\n\
			blakecoin   Fast Blake 256 (8 rounds)\n\
			bmw         BMW 256\n\
			c11/flax    X11 variant\n\
			decred      Decred Blake256\n\
			deep        Deepcoin\n\
			dmd-gr      Diamond-Groestl\n\
			fresh       Freshcoin (shavite 80)\n\
			fugue256    Fuguecoin\n\
			groestl     Groestlcoin\n\
			heavy       Heavycoin\n\
			jackpot     Jackpot\n\
			keccak      Keccak-256 (Maxcoin)\n\
			lbry        LBRY Credits (Sha/Ripemd)\n\
			luffa       Joincoin\n\
			lyra2       CryptoCoin\n\
			lyra2v2     VertCoin\n\
			lyra2Z      ZCoin\n\
			m7          m7 (crytonite) hash\n\
			mjollnir    Mjollnircoin\n\
			myr-gr      Myriad-Groestl\n\
			mtp-classic Zcoin\n\
			mtp-tcr     Tecracoin\n\
			neoscrypt   FeatherCoin, Phoenix, UFO...\n\
			nist5       NIST5 (TalkCoin)\n\
			penta       Pentablake hash (5x Blake 512)\n\
			quark       Quark\n\
			qubit       Qubit\n\
			sia         SIA (Blake2B)\n\
			sib         Sibcoin (X11+Streebog)\n\
			scrypt      Scrypt\n\
			scrypt-jane Scrypt-jane Chacha\n\
			skein       Skein SHA2 (Skeincoin)\n\
			skein2      Double Skein (Woodcoin)\n\
			s3          S3 (1Coin)\n\
			vanilla     Blake256-8 (VNL)\n\
			veltor      Thorsriddle streebog\n\
			whirlcoin   Old Whirlcoin (Whirlpool algo)\n\
			whirlpool   Whirlpool algo\n\
			x11evo      Permuted x11 (Revolver)\n\
			x11         X11 (DarkCoin)\n\
			x13         X13 (MaruCoin)\n\
			x14         X14\n\
			x15         X15\n\
			x17         X17\n\
			zr5         ZR5 (ZiftrCoin)\n\
  -d, --devices         Comma separated list of CUDA devices to use.\n\
                        Device IDs start counting from 0! Alternatively takes\n\
                        string names of your cards like gtx780ti or gt640#2\n\
                        (matching 2nd gt640 in the PC)\n\
  -i  --intensity=N[,N] GPU intensity 8.0-25.0 (default: auto) \n\
                        Decimals are allowed for fine tuning \n\
      --cuda-schedule   Set device threads scheduling mode (default: auto)\n\
  -f, --diff-factor     Divide difficulty by this factor (default 1.0) \n\
  -m, --diff-multiplier Multiply difficulty by this value (default 1.0) \n\
      --vote=VOTE       vote (for decred and HeavyCoin)\n\
      --trust-pool      trust the max block reward vote (maxvote) sent by the pool\n\
  -o, --url=URL         URL of mining server\n\
  -O, --userpass=U:P    username:password pair for mining server\n\
  -u, --user=USERNAME   username for mining server\n\
  -p, --pass=PASSWORD   password for mining server\n\
      --cert=FILE       certificate for mining server using SSL\n\
  -x, --proxy=[PROTOCOL://]HOST[:PORT]  connect through a proxy\n\
  -t, --threads=N       number of miner threads (default: number of nVidia GPUs)\n\
  -r, --retries=N       number of times to retry if a network call fails\n\
                          (default: retry indefinitely)\n\
  -R, --retry-pause=N   time to pause between retries, in seconds (default: 30)\n\
      --shares-limit    maximum shares [s] to mine before exiting the program.\n\
      --time-limit      maximum time [s] to mine before exiting the program.\n\
  -T, --timeout=N       network timeout, in seconds (default: 300)\n\
  -s, --scantime=N      upper bound on time spent scanning current work when\n\
                          long polling is unavailable, in seconds (default: 10)\n\
  -n, --ndevs           list cuda devices\n\
  -N, --statsavg        number of samples used to compute hashrate (default: 30)\n\
      --no-gbt          disable getblocktemplate support (height check in solo)\n\
      --coinbase-addr=ADDR  payout address for solo mining\n\
      --coinbase-sig=TEXT  data to insert in the coinbase when possible\n\
      --no-getwork      disable getwork support\n\
      --no-longpoll     disable X-Long-Polling support\n\
      --no-stratum      disable X-Stratum support\n\
      --no-extranonce   disable extranonce subscribe on stratum\n\
  -q, --quiet           disable per-thread hashmeter output\n\
      --no-color        disable colored output\n\
  -D, --debug           enable debug output\n\
  -P, --protocol-dump   verbose dump of protocol-level activities\n\
      --cpu-affinity    set process affinity to cpu core(s), mask 0x3 for cores 0 and 1\n\
      --cpu-priority    set process priority (default: 3) 0 idle, 2 normal to 5 highest\n\
  -b, --api-bind=port   IP:port for the miner API (default: 127.0.0.1:4068), 0 disabled\n\
      --api-remote      Allow remote control, like pool switching\n\
      --max-temp=N      Only mine if gpu temp is less than specified value\n\
      --max-rate=N[KMG] Only mine if net hashrate is less than specified value\n\
      --max-diff=N      Only mine if net difficulty is less than specified value\n\
                        Can be tuned with --resume-diff=N to set a resume value\n"
#if defined(__linux) || defined(_WIN64) /* via nvml */
"\
      --mem-clock=3505  Set the gpu memory max clock (346.72+ driver)\n\
      --gpu-clock=1150  Set the gpu engine max clock (346.72+ driver)\n\
      --pstate=0[,2]    Set the gpu power state (352.21+ driver)\n\
      --plimit=100W     Set the gpu power limit (352.21+ driver)\n"
#else /* via nvapi.dll */
"\
      --mem-clock=3505  Set the gpu memory boost clock\n\
      --gpu-clock=1150  Set the gpu engine boost clock\n\
      --plimit=100      Set the gpu power limit in percentage\n\
      --tlimit=80       Set the gpu thermal limit in degrees\n\
      --led=100         Set the logo led level (0=disable, 0xFF00FF for RVB)\n"
#endif
#ifdef HAVE_SYSLOG_H
"\
  -S, --syslog          use system log for output messages\n\
      --syslog-prefix=... allow to change syslog tool name\n"
#endif
"\
      --hide-diff       hide submitted block and net difficulty (old mode)\n\
  -B, --background      run the miner in the background\n\
      --benchmark       run in offline benchmark mode\n\
      --cputest         debug hashes from cpu algorithms\n\
  -c, --config=FILE     load a JSON-format configuration file\n\
  -V, --version         display version information and exit\n\
  -h, --help            display this help text and exit\n\
";

static char const short_options[] =
#ifdef HAVE_SYSLOG_H
	"S"
#endif
	"a:Bc:i:Dhp:Px:f:m:nqr:R:s:t:T:o:u:O:Vd:N:b:l:L:";

struct option options[] = {
	{ "algo", 1, NULL, 'a' },
	{ "api-bind", 1, NULL, 'b' },
	{ "api-remote", 0, NULL, 1030 },
	{ "background", 0, NULL, 'B' },
	{ "benchmark", 0, NULL, 1005 },
	{ "cert", 1, NULL, 1001 },
	{ "config", 1, NULL, 'c' },
	{ "cputest", 0, NULL, 1006 },
	{ "no-getwork", 0, NULL, 1010 },
	{ "coinbase-addr", 1, NULL, 1016 },
	{ "coinbase-sig", 1, NULL, 1015 },
	{ "cpu-affinity", 1, NULL, 1020 },
	{ "cpu-priority", 1, NULL, 1021 },
	{ "cuda-schedule", 1, NULL, 1025 },
	{ "debug", 0, NULL, 'D' },
	{ "help", 0, NULL, 'h' },
	{ "intensity", 1, NULL, 'i' },
	{ "ndevs", 0, NULL, 'n' },
	{ "no-color", 0, NULL, 1002 },
	{ "no-extranonce", 0, NULL, 1012 },
	{ "no-gbt", 0, NULL, 1011 },
	{ "no-longpoll", 0, NULL, 1003 },
	{ "no-stratum", 0, NULL, 1007 },
	{ "no-autotune", 0, NULL, 1004 },  // scrypt
	{ "interactive", 1, NULL, 1050 },  // scrypt
	{ "launch-config", 1, NULL, 'l' }, // scrypt
	{ "lookup-gap", 1, NULL, 'L' },    // scrypt
	{ "texture-cache", 1, NULL, 1051 },// scrypt
	{ "max-temp", 1, NULL, 1060 },
	{ "max-diff", 1, NULL, 1061 },
	{ "max-rate", 1, NULL, 1062 },
	{ "resume-diff", 1, NULL, 1063 },
	{ "resume-rate", 1, NULL, 1064 },
	{ "resume-temp", 1, NULL, 1065 },
	{ "pass", 1, NULL, 'p' },
	{ "pool-name", 1, NULL, 1100 },     // pool
	{ "pool-algo", 1, NULL, 1101 },     // pool
	{ "pool-scantime", 1, NULL, 1102 }, // pool
	{ "pool-shares-limit", 1, NULL, 1109 },
	{ "pool-time-limit", 1, NULL, 1108 },
	{ "pool-max-diff", 1, NULL, 1161 }, // pool
	{ "pool-max-rate", 1, NULL, 1162 }, // pool
	{ "pool-disabled", 1, NULL, 1199 }, // pool
	{ "protocol-dump", 0, NULL, 'P' },
	{ "proxy", 1, NULL, 'x' },
	{ "quiet", 0, NULL, 'q' },
	{ "retries", 1, NULL, 'r' },
	{ "retry-pause", 1, NULL, 'R' },
	{ "scantime", 1, NULL, 's' },
	{ "show-diff", 0, NULL, 1013 },
	{ "hide-diff", 0, NULL, 1014 },
	{ "statsavg", 1, NULL, 'N' },
	{ "gpu-clock", 1, NULL, 1070 },
	{ "mem-clock", 1, NULL, 1071 },
	{ "pstate", 1, NULL, 1072 },
	{ "plimit", 1, NULL, 1073 },
	{ "keep-clocks", 0, NULL, 1074 },
	{ "tlimit", 1, NULL, 1075 },
	{ "led", 1, NULL, 1080 },
#ifdef HAVE_SYSLOG_H
	{ "syslog", 0, NULL, 'S' },
	{ "syslog-prefix", 1, NULL, 1018 },
#endif
	{ "shares-limit", 1, NULL, 1009 },
	{ "time-limit", 1, NULL, 1008 },
	{ "threads", 1, NULL, 't' },
	{ "vote", 1, NULL, 1022 },
	{ "trust-pool", 0, NULL, 1023 },
	{ "timeout", 1, NULL, 'T' },
	{ "url", 1, NULL, 'o' },
	{ "user", 1, NULL, 'u' },
	{ "userpass", 1, NULL, 'O' },
	{ "version", 0, NULL, 'V' },
	{ "devices", 1, NULL, 'd' },
	{ "diff-multiplier", 1, NULL, 'm' },
	{ "diff-factor", 1, NULL, 'f' },
	{ "diff", 1, NULL, 'f' }, // compat
	{ "no-donation", 0, NULL, 2001 }, 
	{ "donation", 1, NULL, 2002 }, // compat
	{ 0, 0, 0, 0 }
};

static char const scrypt_usage[] = "\n\
Scrypt specific options:\n\
  -l, --launch-config   gives the launch configuration for each kernel\n\
                        in a comma separated list, one per device.\n\
  -L, --lookup-gap      Divides the per-hash memory requirement by this factor\n\
                        by storing only every N'th value in the scratchpad.\n\
                        Default is 1.\n\
      --interactive     comma separated list of flags (0/1) specifying\n\
                        which of the CUDA device you need to run at inter-\n\
                        active frame rates (because it drives a display).\n\
      --texture-cache   comma separated list of flags (0/1/2) specifying\n\
                        which of the CUDA devices shall use the texture\n\
                        cache for mining. Kepler devices may profit.\n\
      --no-autotune     disable auto-tuning of kernel launch parameters\n\
";

struct work _ALIGN(64) g_work;
volatile time_t g_work_time;
pthread_mutex_t g_work_lock;
static char *lp_id;

// get const array size (defined in ccminer.cpp)
int options_count()
{
	int n = 0;
	while (options[n].name != NULL)
		n++;
	return n;
}

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

static void affine_to_cpu_mask(int id, unsigned long mask) {
	cpu_set_t set;
	CPU_ZERO(&set);
	for (uint8_t i = 0; i < num_cpus; i++) {
		// cpu mask
		if (mask & (1UL<<i)) { CPU_SET(i, &set); }
	}
	if (id == -1) {
		// process affinity
		sched_setaffinity(0, sizeof(&set), &set);
	} else {
		// thread only
		pthread_setaffinity_np(thr_info[id].pth, sizeof(&set), &set);
	}
}
#elif defined(__FreeBSD__) /* FreeBSD specific policy and affinity management */
#include <sys/cpuset.h>
static inline void drop_policy(void) { }
static void affine_to_cpu_mask(int id, unsigned long mask) {
	cpuset_t set;
	CPU_ZERO(&set);
	for (uint8_t i = 0; i < num_cpus; i++) {
		if (mask & (1UL<<i)) CPU_SET(i, &set);
	}
	cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof(cpuset_t), &set);
}
#elif defined(WIN32) /* Windows */
static inline void drop_policy(void) { }
static void affine_to_cpu_mask(int id, unsigned long mask) {
	if (id == -1)
		SetProcessAffinityMask(GetCurrentProcess(), mask);
	else
		SetThreadAffinityMask(GetCurrentThread(), mask);
}
#else /* Martians */
static inline void drop_policy(void) { }
static void affine_to_cpu_mask(int id, uint8_t mask) { }
#endif

static bool get_blocktemplate(CURL *curl, struct work *work);

void get_currentalgo(char* buf, int sz)
{
	snprintf(buf, sz, "%s", algo_names[opt_algo]);
}

/**
 * Exit app
 */
void proper_exit(int reason)
{
	restart_threads();
	if (abort_flag) /* already called */
		return;

	abort_flag = true;
	usleep(200 * 1000);
	cuda_shutdown();

	if (reason == EXIT_CODE_OK && app_exit_code != EXIT_CODE_OK) {
		reason = app_exit_code;
	}

	pthread_mutex_lock(&stats_lock);
	if (check_dups)
		hashlog_purge_all();
	stats_purge_all();
	pthread_mutex_unlock(&stats_lock);

#ifdef WIN32
	timeEndPeriod(1); // else never executed
#endif
#ifdef USE_WRAPNVML
	if (hnvml) {
		for (int n=0; n < opt_n_threads && !opt_keep_clocks; n++) {
			nvml_reset_clocks(hnvml, device_map[n]);
		}
		nvml_destroy(hnvml);
	}
#endif
	free(opt_syslog_pfx);
	free(opt_api_allow);
	//free(work_restart);
	//free(thr_info);
	exit(reason);
}
/*
 bool jobj_binary(const json_t *obj, const char *key,
			void *buf, size_t buflen)
{
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
	if (!hex2bin((uchar*)buf, hexstr, buflen))
		return false;

	return true;
}
*/
/* compute nbits to get the network diff */
static void calc_network_diff(struct work *work)
{
	// sample for diff 43.281 : 1c05ea29
	// todo: endian reversed on longpoll could be zr5 specific...
	uint32_t nbits = (have_longpoll | (opt_algo == ALGO_MTP || opt_algo == ALGO_MTPTCR))? work->data[18] : swab32(work->data[18]);
	if (opt_algo == ALGO_LBRY) nbits = swab32(work->data[26]);
	if (opt_algo == ALGO_DECRED) nbits = work->data[29];
	if (opt_algo == ALGO_SIA) nbits = work->data[11]; // unsure if correct

	uint32_t bits = (nbits & 0xffffff);
	int16_t shift = (swab32(nbits) & 0xff); // 0x1c = 28

	uint64_t diffone = 0x0000FFFF00000000ull;
//	double d = (double)0x0000ffff / (double)bits;
	double d = (double)0x0000ffff / (double)bits;
	for (int m=shift; m < 29; m++) d *= 256.0;
	for (int m=29; m < shift; m++) d /= 256.0;
	if (opt_algo == ALGO_DECRED && shift == 28) d *= 256.0;
	if (opt_debug_diff)
		applog(LOG_DEBUG, "net diff: %f -> shift %u, bits %08x", d, shift, bits);

	net_diff = d;
}

/* decode data from getwork (wallets and longpoll pools) */
static bool work_decode(const json_t *val, struct work *work)
{
	int data_size, target_size = sizeof(work->target);
	int adata_sz, atarget_sz = ARRAY_SIZE(work->target);
	int i;

	switch (opt_algo) {
	case ALGO_M7:
		data_size = 122;
		adata_sz = data_size / 4;
		break;
	case ALGO_DECRED:
		data_size = 192;
		adata_sz = 180/4;
		break;
	case ALGO_NEOSCRYPT:
	case ALGO_ZR5:
		data_size = 80;
		adata_sz = data_size / 4;
		break;
	default:
		data_size = 128;
		adata_sz = data_size / 4;
	}

	if (!jobj_binary(val, "data", work->data, data_size)) {
		json_t *obj = json_object_get(val, "data");
		int len = obj ? (int) strlen(json_string_value(obj)) : 0;
		if (!len || len > sizeof(work->data)*2) {
			applog(LOG_ERR, "JSON invalid data (len %d <> %d)", len/2, data_size);
			return false;
		} else {
			data_size = len / 2;
			if (!jobj_binary(val, "data", work->data, data_size)) {
				applog(LOG_ERR, "JSON invalid data (len %d)", data_size);
				return false;
			}
		}
	}

	if (!jobj_binary(val, "target", work->target, target_size)) {
		applog(LOG_ERR, "JSON invalid target");
		return false;
	}

	if (opt_algo == ALGO_HEAVY) {
		if (unlikely(!jobj_binary(val, "maxvote", &work->maxvote, sizeof(work->maxvote)))) {
			work->maxvote = 2048;
		}
	} else work->maxvote = 0;

	for (i = 0; i < adata_sz; i++)
		work->data[i] = le32dec(work->data + i);
	for (i = 0; i < atarget_sz; i++)
		work->target[i] = le32dec(work->target + i);

	if ((opt_showdiff || opt_max_diff > 0.) && !allow_mininginfo)
		calc_network_diff(work);

	work->targetdiff = target_to_diff(work->target);

	// for api stats, on longpoll pools
	stratum_diff = work->targetdiff;

	work->tx_count = use_pok = 0;
	if (opt_algo == ALGO_ZR5 && work->data[0] & POK_BOOL_MASK) {
		use_pok = 1;
		json_t *txs = json_object_get(val, "txs");
		if (txs && json_is_array(txs)) {
			size_t idx, totlen = 0;
			json_t *p;

			json_array_foreach(txs, idx, p) {
				const int tx = work->tx_count % POK_MAX_TXS;
				const char* hexstr = json_string_value(p);
				size_t txlen = strlen(hexstr)/2;
				work->tx_count++;
				if (work->tx_count > POK_MAX_TXS || txlen >= POK_MAX_TX_SZ) {
					// when tx is too big, just reset use_pok for the block
					use_pok = 0;
					if (opt_debug) applog(LOG_WARNING,
						"pok: large block ignored, tx len: %u", txlen);
					work->tx_count = 0;
					break;
				}
				hex2bin((uchar*)work->txspok[tx].data, hexstr, min(txlen, POK_MAX_TX_SZ));
				work->txspok[tx].len = (uint32_t) (txlen);
				totlen += txlen;
			}
			if (opt_debug)
				applog(LOG_DEBUG, "block txs: %u, total len: %u", work->tx_count, totlen);
		}
	}

	/* use work ntime as job id (solo-mining) */
	cbin2hex(work->job_id, (const char*)&work->data[17], 4);

	if (opt_algo == ALGO_DECRED) {
		uint16_t vote;
		// always keep last bit of votebits
		memcpy(&vote, &work->data[25], 2);
		vote = (opt_vote << 1) | (vote & 1);
		memcpy(&work->data[25], &vote, 2);
		// some random extradata to make it unique
		work->data[36] = (rand()*4);
		work->data[37] = (rand()*4) << 8;
		// required for the longpoll pool block info...
		work->height = work->data[32];
		if (!have_longpoll && work->height > net_blocks + 1) {
			char netinfo[64] = { 0 };
			if (opt_showdiff && net_diff > 0.) {
				if (net_diff != work->targetdiff)
					sprintf(netinfo, ", diff %.3f, pool %.1f", net_diff, work->targetdiff);
				else
					sprintf(netinfo, ", diff %.3f", net_diff);
			}
			applog(LOG_BLUE, "%s block %d%s",
				algo_names[opt_algo], work->height, netinfo);
			net_blocks = work->height - 1;
		}
		cbin2hex(work->job_id, (const char*)&work->data[34], 4);
	}

	return true;
}

#define YES "yes!"
#define YAY "yay!!!"
#define BOO "booooo"

int share_result(int result, int pooln, double sharediff, const char *reason)
{
	const char *flag;
	char suppl[32] = { 0 };
	char s[32] = { 0 };
	double hashrate = 0.;
	struct pool_infos *p = &pools[pooln];

	pthread_mutex_lock(&stats_lock);
	for (int i = 0; i < opt_n_threads; i++) {
		hashrate += stats_get_speed(i, thr_hashrates[i]);
	}
	pthread_mutex_unlock(&stats_lock);

	result ? p->accepted_count++ : p->rejected_count++;

	p->last_share_time = time(NULL);
	if (sharediff > p->best_share)
		p->best_share = sharediff;

	global_hashrate = llround(hashrate);

	format_hashrate(hashrate, s);
	if (opt_showdiff)
		sprintf(suppl, "diff %.3f", sharediff);
	else // accepted percent
		sprintf(suppl, "%.2f%%", 100. * p->accepted_count / (p->accepted_count + p->rejected_count));

	if (!net_diff || sharediff < net_diff) {
		flag = use_colors ?
			(result ? CL_GRN YES : CL_RED BOO)
		:	(result ? "(" YES ")" : "(" BOO ")");
	} else {
		p->solved_count++;
		flag = use_colors ?
			(result ? CL_GRN YAY : CL_RED BOO)
		:	(result ? "(" YAY ")" : "(" BOO ")");
	}

	applog(LOG_NOTICE, "accepted: %lu/%lu (%s), %s %s",
			p->accepted_count,
			p->accepted_count + p->rejected_count,
			suppl, s, flag);
	if (reason) {
		applog(LOG_WARNING, "reject reason: %s", reason);
		if (!check_dups && strncasecmp(reason, "duplicate", 9) == 0) {
			applog(LOG_WARNING, "enabling duplicates check feature");
			check_dups = true;
			g_work_time = 0;
		}
	}
	return 1;
}

static bool submit_upstream_work(CURL *curl, struct work *work)
{
	char s[512];
	struct pool_infos *pool = &pools[work->pooln];
	json_t *val, *res, *reason;
	bool stale_work = false;
	int idnonce = 0;

	/* discard if a newer block was received */
/*
	stale_work = work->height && work->height < g_work.height;
	if (have_stratum && !stale_work && opt_algo != ALGO_ZR5 && opt_algo != ALGO_SCRYPT_JANE) {
		pthread_mutex_lock(&g_work_lock);
		if (strlen(work->job_id + 8))
			stale_work = strncmp(work->job_id + 8, g_work.job_id + 8, sizeof(g_work.job_id) - 8);
		if (stale_work) {
			pool->stales_count++;
			if (opt_debug) applog(LOG_DEBUG, "outdated job %s, new %s stales=%d",
				work->job_id + 8 , g_work.job_id + 8, pool->stales_count);
			if (!check_stratum_jobs && pool->stales_count > 5) {
				if (!opt_quiet) applog(LOG_WARNING, "Enabled stratum stale jobs workaround");
				check_stratum_jobs = true;
			}
		}
		pthread_mutex_unlock(&g_work_lock);
	}

	if (!have_stratum && !stale_work && allow_gbt) {
		struct work wheight = { 0 };
		if (get_blocktemplate(curl, &wheight)) {
			if (work->height && work->height < wheight.height) {
				if (opt_debug)
					applog(LOG_WARNING, "block %u was already solved", work->height);
				return true;
			}
		}
	}

	if (!stale_work && opt_algo == ALGO_ZR5 && !have_stratum) {
		stale_work = (memcmp(&work->data[1], &g_work.data[1], 68));
	}

	if (!submit_old && stale_work) {
		if (opt_debug)
			applog(LOG_WARNING, "stale work detected, discarding");
		return true;
	}
*/
	if (pool->type & POOL_STRATUM) {
		uint32_t sent = 0;
		uint32_t ntime, nonce;
		char *ntimestr, *noncestr, *xnonce2str, *nvotestr;
		uint16_t nvote = 0;

		switch (opt_algo) {
		case ALGO_M7:
			be64enc(&ntime, ((uint64_t*)work->data)[12]);
			be32enc(&nonce, work->data[29]);
			break;
		case ALGO_BLAKE:
		case ALGO_BLAKECOIN:
		case ALGO_BLAKE2S:
		case ALGO_BMW:
		case ALGO_VANILLA:
			// fast algos require that... (todo: regen hash)
			check_dups = true;
			le32enc(&ntime, work->data[17]);
			le32enc(&nonce, work->data[19]);
			break;
		case ALGO_DECRED:
			be16enc(&nvote, *((uint16_t*)&work->data[25]));
			be32enc(&ntime, work->data[34]);
			be32enc(&nonce, work->data[35]);
			break;
		case ALGO_HEAVY:
			le32enc(&ntime, work->data[17]);
			le32enc(&nonce, work->data[19]);
			be16enc(&nvote, *((uint16_t*)&work->data[20]));
			break;
		case ALGO_LBRY:
			check_dups = true;
			le32enc(&ntime, work->data[25]);
			le32enc(&nonce, work->data[27]);
			break;
		case ALGO_SIA:
			be32enc(&ntime, work->data[10]);
			be32enc(&nonce, work->data[8]);
			break;
		case ALGO_ZR5:
			check_dups = true;
			be32enc(&ntime, work->data[17]);
			be32enc(&nonce, work->data[19]);
			break;
		default:
			le32enc(&ntime, work->data[17]);
			le32enc(&nonce, work->data[19]);
		}
		noncestr = bin2hex((const uchar*)(&nonce), 4);

		if (check_dups)
			sent = hashlog_already_submittted(work->job_id, nonce);
		if (sent > 0) {
//		printf("sent > 0 \n");
			sent = (uint32_t) time(NULL) - sent;
			if (!opt_quiet) {
				applog(LOG_WARNING, "nonce %s was already sent %u seconds ago", noncestr, sent);
				hashlog_dump_job(work->job_id);
			}
			free(noncestr);
			// prevent useless computing on some pools
			g_work_time = 0;
			restart_threads();
			return true;
		}
		if (opt_algo == ALGO_M7)
			ntimestr = bin2hex((const uchar*)(&ntime), 8);
		else 
			ntimestr = bin2hex((const uchar*)(&ntime), 4);

		if (opt_algo == ALGO_DECRED) {
			xnonce2str = bin2hex((const uchar*)&work->data[36], stratum.xnonce1_size);
		} else if (opt_algo == ALGO_SIA) {
			uint16_t high_nonce = swab32(work->data[9]) >> 16;
			xnonce2str = bin2hex((unsigned char*)(&high_nonce), 2);
		} else {
			xnonce2str = bin2hex(work->xnonce2, work->xnonce2_len);
		}

		// store to keep/display the solved ratio/diff
		stratum.sharediff = work->sharediff[idnonce];

		if (net_diff && stratum.sharediff > net_diff && (opt_debug || opt_debug_diff))
			applog(LOG_INFO, "share diff: %.5f, possible block found!!!",
				stratum.sharediff);
		else if (opt_debug_diff)
			applog(LOG_DEBUG, "share diff: %.5f (x %.1f)",
				stratum.sharediff, work->shareratio);

		if (opt_vote) { // ALGO_HEAVY ALGO_DECRED
			nvotestr = bin2hex((const uchar*)(&nvote), 2);
			sprintf(s, "{\"method\": \"mining.submit\", \"params\": ["
					"\"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\"], \"id\":%d}",
					pool->user, work->job_id + 8, xnonce2str, ntimestr, noncestr, nvotestr, 10+idnonce);
			free(nvotestr);
		} else {
			sprintf(s, "{\"method\": \"mining.submit\", \"params\": ["
					"\"%s\", \"%s\", \"%s\", \"%s\", \"%s\"], \"id\":%d}",
					pool->user, work->job_id + 8, xnonce2str, ntimestr, noncestr, 10+idnonce);
		}
		free(xnonce2str);
		free(ntimestr);
		free(noncestr);

		gettimeofday(&stratum.tv_submit, NULL);
		if (unlikely(!stratum_send_line(&stratum, s))) {
			applog(LOG_ERR, "submit_upstream_work stratum_send_line failed");
			return false;
		}

		if (check_dups)
			hashlog_remember_submit(work, nonce);

	} else if (work->txs) { /* gbt */

	char data_str[2 * sizeof(work->data) + 1];
	char *req;
 
	for (int i = 0; i < ARRAY_SIZE(work->data); i++)
		be32enc(work->data + i, work->data[i]);
	dbin2hex(data_str, (unsigned char *)work->data, 80);
	if (work->workid) {
		char *params;
		val = json_object();
		json_object_set_new(val, "workid", json_string(work->workid));
		params = json_dumps(val, 0);
		json_decref(val);

		req = (char*)malloc(128 + 2 * 80 + strlen(work->txs) + strlen(params));
		sprintf(req,
			"{\"method\": \"submitblock\", \"params\": [\"%s%s\", %s], \"id\":4}\r\n",
			data_str, work->txs, params);
		free(params);
 
	}
	else {


		req = (char*)malloc(128 + 2 * 80 + strlen(work->txs));
		sprintf(req,
			"{\"method\": \"submitblock\", \"params\": [\"%s%s\"], \"id\":4}\r\n",
			data_str, work->txs);
 
	}

//	val = json_rpc_call(curl, rpc_url, rpc_userpass, req, NULL, 0);
	val = json_rpc_call_pool(curl,  pool, req, false,false, NULL);
	free(req);
	if (unlikely(!val)) {
		applog(LOG_ERR, "submit_upstream_work json_rpc_call failed");
		return false;
	}

	res = json_object_get(val, "result");
	if (json_is_object(res)) {
		char *res_str;
		bool sumres = false;
		void *iter = json_object_iter(res);
		while (iter) {
			if (json_is_null(json_object_iter_value(iter))) {
				sumres = true;
				break;
			}
			iter = json_object_iter_next(res, iter);
		}
		res_str = json_dumps(res, 0);
		share_result(sumres, work->pooln,work->sharediff[0], res_str);
		free(res_str);
	}
	else
		share_result(json_is_null(res), work->pooln,work->sharediff[0], json_string_value(res));

	json_decref(val);

		} else {

		int data_size = 128;
		int adata_sz = data_size / sizeof(uint32_t);

		/* build hex string */
		char *str = NULL;
		if (opt_algo == ALGO_M7) {
			data_size = 122; adata_sz = data_size/4;
		}
		else if (opt_algo == ALGO_ZR5) {
			data_size = 80; adata_sz = 20;
		}
		else if (opt_algo == ALGO_DECRED) {
			data_size = 192; adata_sz = 180/4;
		}
		else if (opt_algo == ALGO_SIA) {
			return sia_submit(curl, pool, work);
		}

		if (opt_algo != ALGO_HEAVY && opt_algo != ALGO_MJOLLNIR) {
			for (int i = 0; i < adata_sz; i++)
				le32enc(work->data + i, work->data[i]);
		}
		str = bin2hex((uchar*)work->data, data_size);
		if (unlikely(!str)) {
			applog(LOG_ERR, "submit_upstream_work OOM");
			return false;
		}

		/* build JSON-RPC request */
		sprintf(s,
			"{\"method\": \"getwork\", \"params\": [\"%s\"], \"id\":10}\r\n",
			str);

		/* issue JSON-RPC request */
		val = json_rpc_call_pool(curl, pool, s, false, false, NULL);
		if (unlikely(!val)) {
			applog(LOG_ERR, "submit_upstream_work json_rpc_call failed");
			return false;
		}

		res = json_object_get(val, "result");
		reason = json_object_get(val, "reject-reason");
		if (!share_result(json_is_true(res), work->pooln, work->sharediff[0],
				reason ? json_string_value(reason) : NULL))
		{
			if (check_dups)
				hashlog_purge_job(work->job_id);
		}

		json_decref(val);

		free(str);
	}
 
	return true;
}

static bool submit_upstream_work_mtp(CURL *curl, struct work *work, struct mtp *mtp)
{
//	restart_threads();
	const uint8_t MTPC_L = 64;
	
	char s[512];
	struct pool_infos *pool = &pools[work->pooln];
	json_t *val, *res, *reason;
	bool stale_work = false;
	int idnonce = 0;

	uint32_t SizeMerkleRoot = 16;
	uint32_t SizeReserved = 64;
	uint32_t SizeMtpHash = 32;
	uint32_t SizeBlockMTP = MTPC_L *2*128*8;
	uint32_t SizeProofMTP = MTPC_L *3*353;
//printf("rpc user %s\n",rpc_user);
	
		if (pool->type & POOL_STRATUM) {

			uint32_t sent = 0;
			uint32_t ntime, nonce;
			char *ntimestr, *noncestr, *xnonce2str, *nvotestr;

			le32enc(&ntime, work->data[17]);
			le32enc(&nonce, work->data[19]);
			char *sobid = (char*)malloc(9);
			sprintf(sobid,"%s",work->job_id+8);

		json_t *MyObject = json_object();
		json_t *json_arr = json_array();
		json_object_set(MyObject, "id", json_integer(4));
		json_object_set(MyObject, "method", json_string("mining.submit"));

		json_array_append(json_arr, json_string(rpc_user));

		int Err = 0;
	
		uchar hexjob_id[4]; // = (uchar*)malloc(4);
		hex2bin((uchar*)&hexjob_id, sobid, 4);
//		printf("the submitted job id = %s \n",sobid);
		free(sobid);

		json_array_append(json_arr, json_bytes((uchar*)&hexjob_id, 4));		
		json_array_append(json_arr, json_bytes(work->xnonce2, sizeof(uint64_t)));
		json_array_append(json_arr, json_bytes((uchar*)&ntime, sizeof(uint32_t)));
		json_array_append(json_arr, json_bytes((uchar*)&nonce, sizeof(uint32_t)));
		json_array_append(json_arr, json_bytes(mtp->MerkleRoot, SizeMerkleRoot));
		json_array_append(json_arr, json_bytes((uchar*)mtp->nBlockMTP, SizeBlockMTP));
		json_array_append(json_arr, json_bytes(mtp->nProofMTP, SizeProofMTP));

		json_object_set(MyObject, "params", json_arr);

		json_error_t *boserror = (json_error_t *)malloc(sizeof(json_error_t));
		bos_t *serialized = bos_serialize(MyObject, boserror);

		stratum.sharediff = work->sharediff[0];

		if (unlikely(!stratum_send_line_bos(&stratum, serialized))) {
			applog(LOG_ERR, "submit_upstream_work stratum_send_line failed");
			free(boserror);
			json_decref(MyObject);
			bos_free(serialized);
			return false;
		}
		free(boserror);
		json_decref(MyObject);
		bos_free(serialized);
		//		stratum_recv_line_compact(&stratum);

//		free(mtp);
		return true;

	}
	else if (work->txs) { /* gbt */




	int data_size = 84;
//	int data_size = 128;
	int adata_sz = data_size / sizeof(uint32_t);

	char *proofmtp_str = (char*)malloc(2*SizeProofMTP+1);
	char *blockmtp_str = (char*)malloc(2*SizeBlockMTP+1);
	char *merkleroot_str = (char*)malloc(2*SizeMerkleRoot+1);
	char *mtphashvalue_str = (char*)malloc(2 * SizeMtpHash + 1);
	char *mtpreserved_str = (char*)malloc(2*SizeReserved+1);

	for (uint32_t i = 0; i < SizeMerkleRoot; i++)
	{
		sprintf(&merkleroot_str[2*i], "%02x", mtp->MerkleRoot[i]);
	}

	for (uint32_t i = 0; i < SizeMtpHash; i++)
	{
		sprintf(&mtphashvalue_str[2 * i], "%02x", mtp->mtpHashValue[i]);
	}


	for (uint32_t i = 0; i < SizeReserved; i++)
	{
		sprintf(&mtpreserved_str[2 * i], "%02x", 0);
	}


	for (uint32_t i = 0; i < MTPC_L *2; i++)
	{
		for (uint32_t j = 0; j < 1024; j++)
			sprintf(&blockmtp_str[2*(j + 1024*i)], "%02x", ((uchar*)mtp->nBlockMTP[i])[j]);
	}


	
	for (int i = 0;i< SizeProofMTP;i++)
	sprintf(&proofmtp_str[2*i],"%02x",mtp->nProofMTP[i]);



		char data_str[2 * sizeof(work->data) + 1];
		char data_check[2* sizeof(work->data)+1];
		char *req;
	
		for (int i = 0; i < ARRAY_SIZE(work->data); i++)
			le32enc(work->data + i, work->data[i]);

		dbin2hex(data_str, (unsigned char *)work->data, 84);
		
		for (int i=0;i<84;i++)
		sprintf(&data_check[2*i],"%02x",((uint8_t*)work->data)[i]);
		

		if (work->workid) {
			char *params;
			val = json_object();
			json_object_set_new(val, "workid", json_string(work->workid));
			params = json_dumps(val, 0);
			json_decref(val);

			req = (char*)malloc(128 + 2 * 84 + strlen(work->txs) + strlen(params) + strlen(mtphashvalue_str) 
									+ strlen(mtpreserved_str)  + strlen(merkleroot_str)+ strlen(proofmtp_str) + strlen(blockmtp_str));
			sprintf(req,
				"{\"method\": \"submitblock\", \"params\": [\"%s%s%s%s%s%s%s\", %s], \"id\":4}\r\n",
				data_str, mtphashvalue_str, mtpreserved_str, merkleroot_str, blockmtp_str, proofmtp_str, work->txs, params);
			free(params);

		}
		else {
			req = (char*)malloc(128 + 2 * 84 + strlen(work->txs) + strlen(mtphashvalue_str) + strlen(mtpreserved_str)  
											 + strlen(merkleroot_str) + strlen(proofmtp_str) + strlen(blockmtp_str) );
			sprintf(req,
				"{\"method\": \"submitblock\", \"params\": [\"%s%s%s%s%s%s%s\"], \"id\":4}\r\n",
				data_str, mtphashvalue_str, mtpreserved_str, merkleroot_str, blockmtp_str, proofmtp_str, work->txs);
		}

//		val = json_rpc_call(curl, rpc_url, rpc_userpass, req, NULL, 0);
//		printf("work->txs=%s\n",work->txs);
		val = json_rpc_call_pool(curl, pool, req, false, false, NULL);
//		printf("not submitting block\n");
		free(req);
		if (unlikely(!val)) {
			applog(LOG_ERR, "submit_upstream_work json_rpc_call failed");
			return false;
		}
		
		res = json_object_get(val, "result");
		if (json_is_object(res)) {
			char *res_str;
			bool sumres = false;
			void *iter = json_object_iter(res);
			while (iter) {
				if (json_is_null(json_object_iter_value(iter))) {
					sumres = true;
					break;
				}
				iter = json_object_iter_next(res, iter);
			}
			res_str = json_dumps(res, 0);
			share_result(sumres, work->pooln, work->sharediff[0], res_str);
			free(res_str);
		}
		else
			share_result(json_is_null(res), work->pooln, work->sharediff[0], json_string_value(res));

		json_decref(val);
		free(proofmtp_str); 
		free(blockmtp_str); 
		free(merkleroot_str);
		free(mtpreserved_str);
		free(mtphashvalue_str);
	}
//free(proof_str);


	return true;
}


static bool submit_upstream_work_mtptcr(CURL *curl, struct work *work, struct mtp *mtp)
{
	//	restart_threads();
	const uint8_t MTPC_L = 16;

	char s[512];
	struct pool_infos *pool = &pools[work->pooln];
	json_t *val, *res, *reason;
	bool stale_work = false;
	int idnonce = 0;

	uint32_t SizeMerkleRoot = 16;
	uint32_t SizeReserved = 64;
	uint32_t SizeMtpHash = 32;
	uint32_t SizeBlockMTP = MTPC_L * 2 * 128 * 8;
	uint32_t SizeProofMTP = MTPC_L * 3 * 353;
	//printf("rpc user %s\n",rpc_user);





	if (pool->type & POOL_STRATUM) {

		uint32_t sent = 0;
		uint32_t ntime, nonce;
		char *ntimestr, *noncestr, *xnonce2str, *nvotestr;

		le32enc(&ntime, work->data[17]);
		le32enc(&nonce, work->data[19]);
		char *sobid = (char*)malloc(9);
		sprintf(sobid, "%s", work->job_id + 8);

		json_t *MyObject = json_object();
		json_t *json_arr = json_array();
		json_object_set(MyObject, "id", json_integer(4));
		json_object_set(MyObject, "method", json_string("mining.submit"));

		json_array_append(json_arr, json_string(rpc_user));

		int Err = 0;

		uchar hexjob_id[4]; // = (uchar*)malloc(4);
		hex2bin((uchar*)&hexjob_id, sobid, 4);
		//		printf("the submitted job id = %s \n",sobid);
		free(sobid);

		json_array_append(json_arr, json_bytes((uchar*)&hexjob_id, 4));
		json_array_append(json_arr, json_bytes(work->xnonce2, sizeof(uint64_t)));
		json_array_append(json_arr, json_bytes((uchar*)&ntime, sizeof(uint32_t)));
		json_array_append(json_arr, json_bytes((uchar*)&nonce, sizeof(uint32_t)));
		json_array_append(json_arr, json_bytes(mtp->MerkleRoot, SizeMerkleRoot));
		json_array_append(json_arr, json_bytes((uchar*)mtp->nBlockMTP, SizeBlockMTP));
		json_array_append(json_arr, json_bytes(mtp->nProofMTP, SizeProofMTP));

		json_object_set(MyObject, "params", json_arr);

		json_error_t *boserror = (json_error_t *)malloc(sizeof(json_error_t));
		bos_t *serialized = bos_serialize(MyObject, boserror);

		stratum.sharediff = work->sharediff[0];

		if (unlikely(!stratum_send_line_bos(&stratum, serialized))) {
			applog(LOG_ERR, "submit_upstream_work stratum_send_line failed");
			free(boserror);
			json_decref(MyObject);
			bos_free(serialized);
			return false;
		}
		free(boserror);
		json_decref(MyObject);
		bos_free(serialized);
		//		stratum_recv_line_compact(&stratum);

		//		free(mtp);
		return true;

	}
	else if (work->txs) { /* gbt */




		int data_size = 84;
		//	int data_size = 128;
		int adata_sz = data_size / sizeof(uint32_t);

		char *proofmtp_str = (char*)malloc(2 * SizeProofMTP + 1);
		char *blockmtp_str = (char*)malloc(2 * SizeBlockMTP + 1);
		char *merkleroot_str = (char*)malloc(2 * SizeMerkleRoot + 1);
		char *mtphashvalue_str = (char*)malloc(2 * SizeMtpHash + 1);
		char *mtpreserved_str = (char*)malloc(2 * SizeReserved + 1);

		for (uint32_t i = 0; i < SizeMerkleRoot; i++)
		{
			sprintf(&merkleroot_str[2 * i], "%02x", mtp->MerkleRoot[i]);
		}

		for (uint32_t i = 0; i < SizeMtpHash; i++)
		{
			sprintf(&mtphashvalue_str[2 * i], "%02x", mtp->mtpHashValue[i]);
		}


		for (uint32_t i = 0; i < SizeReserved; i++)
		{
			sprintf(&mtpreserved_str[2 * i], "%02x", 0);
		}


		for (uint32_t i = 0; i < MTPC_L * 2; i++)
		{
			for (uint32_t j = 0; j < 1024; j++)
				sprintf(&blockmtp_str[2 * (j + 1024 * i)], "%02x", ((uchar*)mtp->nBlockMTP[i])[j]);
		}



		for (int i = 0; i< SizeProofMTP; i++)
			sprintf(&proofmtp_str[2 * i], "%02x", mtp->nProofMTP[i]);



		char data_str[2 * sizeof(work->data) + 1];
		char data_check[2 * sizeof(work->data) + 1];
		char *req;

		for (int i = 0; i < ARRAY_SIZE(work->data); i++)
			le32enc(work->data + i, work->data[i]);

		dbin2hex(data_str, (unsigned char *)work->data, 84);

		for (int i = 0; i<84; i++)
			sprintf(&data_check[2 * i], "%02x", ((uint8_t*)work->data)[i]);


		if (work->workid) {
			char *params;
			val = json_object();
			json_object_set_new(val, "workid", json_string(work->workid));
			params = json_dumps(val, 0);
			json_decref(val);

			req = (char*)malloc(128 + 2 * 84 + strlen(work->txs) + strlen(params) + strlen(mtphashvalue_str)
				+ strlen(mtpreserved_str) + strlen(merkleroot_str) + strlen(proofmtp_str) + strlen(blockmtp_str));
			sprintf(req,
				"{\"method\": \"submitblock\", \"params\": [\"%s%s%s%s%s%s%s\", %s], \"id\":4}\r\n",
				data_str, mtphashvalue_str, mtpreserved_str, merkleroot_str, blockmtp_str, proofmtp_str, work->txs, params);
			free(params);

		}
		else {
			req = (char*)malloc(128 + 2 * 84 + strlen(work->txs) + strlen(mtphashvalue_str) + strlen(mtpreserved_str)
				+ strlen(merkleroot_str) + strlen(proofmtp_str) + strlen(blockmtp_str));
			sprintf(req,
				"{\"method\": \"submitblock\", \"params\": [\"%s%s%s%s%s%s%s\"], \"id\":4}\r\n",
				data_str, mtphashvalue_str, mtpreserved_str, merkleroot_str, blockmtp_str, proofmtp_str, work->txs);
		}

		//		val = json_rpc_call(curl, rpc_url, rpc_userpass, req, NULL, 0);
		//		printf("work->txs=%s\n",work->txs);
		val = json_rpc_call_pool(curl, pool, req, false, false, NULL);
		//		printf("not submitting block\n");
		free(req);
		if (unlikely(!val)) {
			applog(LOG_ERR, "submit_upstream_work json_rpc_call failed");
			return false;
		}

		res = json_object_get(val, "result");
		if (json_is_object(res)) {
			char *res_str;
			bool sumres = false;
			void *iter = json_object_iter(res);
			while (iter) {
				if (json_is_null(json_object_iter_value(iter))) {
					sumres = true;
					break;
				}
				iter = json_object_iter_next(res, iter);
			}
			res_str = json_dumps(res, 0);
			share_result(sumres, work->pooln, work->sharediff[0], res_str);
			free(res_str);
		}
		else
			share_result(json_is_null(res), work->pooln, work->sharediff[0], json_string_value(res));

		json_decref(val);
		free(proofmtp_str);
		free(blockmtp_str);
		free(merkleroot_str);
		free(mtpreserved_str);
		free(mtphashvalue_str);
	}
	//free(proof_str);


	return true;
}



#define BLOCK_VERSION_CURRENT 3
// to fix
static bool gbt_work_decode(const json_t *val, struct work *work)
{
	int i, n;
	uint32_t version, curtime, bits;
	uint32_t prevhash[8];
	uint32_t target[8];
	int cbtx_size;
	uchar *cbtx = NULL;
	int tx_count, tx_size;
	uchar txc_vi[9];
	uchar(*merkle_tree)[32] = NULL;
	bool coinbase_append = false;
	bool submit_coinbase = false;
	bool version_force = false;
	bool version_reduce = false;
	json_t *tmp, *txa;
	bool rc = false;

	tmp = json_object_get(val, "mutable");
	if (tmp && json_is_array(tmp)) {
		n = (int)json_array_size(tmp);
		for (i = 0; i < n; i++) {
			const char *s = json_string_value(json_array_get(tmp, i));
			if (!s)
				continue;
			if (!strcmp(s, "coinbase/append"))
				coinbase_append = true;
			else if (!strcmp(s, "submit/coinbase"))
				submit_coinbase = true;
			else if (!strcmp(s, "version/force"))
				version_force = true;
			else if (!strcmp(s, "version/reduce"))
				version_reduce = true;
		}
	}

	tmp = json_object_get(val, "height");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid height");
		goto out;
	}
	work->height = (int)json_integer_value(tmp);
	applog(LOG_BLUE, "Current block is %d", work->height);

	tmp = json_object_get(val, "version");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid version");
		goto out;
	}
	version = (uint32_t)json_integer_value(tmp);
	if ((version & 0xffU) > BLOCK_VERSION_CURRENT) {
		if (version_reduce) {
			version = (version & ~0xffU) | BLOCK_VERSION_CURRENT;
		}
		else if (have_gbt && allow_getwork && !version_force) {
			applog(LOG_DEBUG, "Switching to getwork, gbt version %d", version);
			have_gbt = false;
			goto out;
		}
		else if (!version_force) {
			applog(LOG_ERR, "Unrecognized block version: %u", version);
			goto out;
		}
	}

	if (unlikely(!jobj_binary(val, "previousblockhash", prevhash, sizeof(prevhash)))) {
		applog(LOG_ERR, "JSON invalid previousblockhash");
		goto out;
	}

	tmp = json_object_get(val, "curtime");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid curtime");
		goto out;
	}
	curtime = (uint32_t)json_integer_value(tmp);

	if (unlikely(!jobj_binary(val, "bits", &bits, sizeof(bits)))) {
		applog(LOG_ERR, "JSON invalid bits");
		goto out;
	}

	// find count and size of transactions 
	txa = json_object_get(val, "transactions");
	if (!txa || !json_is_array(txa)) {
		applog(LOG_ERR, "JSON invalid transactions");
		goto out;
	}
	tx_count = (int)json_array_size(txa);
	tx_size = 0;
	for (i = 0; i < tx_count; i++) {
		const json_t *tx = json_array_get(txa, i);
		const char *tx_hex = json_string_value(json_object_get(tx, "data"));
		if (!tx_hex) {
			applog(LOG_ERR, "JSON invalid transactions");
			goto out;
		}
		tx_size += (int)(strlen(tx_hex) / 2);
	}

	// build coinbase transaction 
	tmp = json_object_get(val, "coinbasetxn");
	if (tmp) {
//printf("printf coinbase txn\n");
		const char *cbtx_hex = json_string_value(json_object_get(tmp, "data"));
		cbtx_size = cbtx_hex ? (int)strlen(cbtx_hex) / 2 : 0;
		cbtx = (uchar*)malloc(cbtx_size + 100);
		if (cbtx_size < 60 || !hex2bin(cbtx, cbtx_hex, cbtx_size)) {
			applog(LOG_ERR, "JSON invalid coinbasetxn");
			goto out;
		}
	}
	else {
		int64_t cbvalue;
		if (!pk_script_size) {
			if (allow_getwork) {
				applog(LOG_INFO, "No payout address provided, switching to getwork");
				have_gbt = false;
			}
			else
				applog(LOG_ERR, "No payout address provided");
			goto out;
		}
		tmp = json_object_get(val, "coinbasevalue");
		if (!tmp || !json_is_number(tmp)) {
			applog(LOG_ERR, "JSON invalid coinbasevalue");
			goto out;
		}
		cbvalue = (int64_t)(json_is_integer(tmp) ? json_integer_value(tmp) : json_number_value(tmp));
		cbtx = (uchar*)malloc(256);
		le32enc((uint32_t *)cbtx, 1); // version /
		cbtx[4] = 1; // in-counter /
		memset(cbtx + 5, 0x00, 32); // prev txout hash /
		le32enc((uint32_t *)(cbtx + 37), 0xffffffff); // prev txout index /
		cbtx_size = 43;
		// BIP 34: height in coinbase /
		for (n = work->height; n; n >>= 8)
			cbtx[cbtx_size++] = n & 0xff;
		cbtx[42] = cbtx_size - 43;
		cbtx[41] = cbtx_size - 42; // scriptsig length /
		le32enc((uint32_t *)(cbtx + cbtx_size), 0xffffffff); // sequence /
		cbtx_size += 4;

		cbtx[cbtx_size++] = 1; // out-counter /

		le32enc((uint32_t *)(cbtx + cbtx_size), (uint32_t)cbvalue); // value /
		le32enc((uint32_t *)(cbtx + cbtx_size + 4), cbvalue >> 32);
		cbtx_size += 8;
		cbtx[cbtx_size++] = (uint8_t)pk_script_size; // txout-script length /
		memcpy(cbtx + cbtx_size, pk_script, pk_script_size);
		cbtx_size += (int)pk_script_size;

		le32enc((uint32_t *)(cbtx + cbtx_size), 0); // lock time /
		cbtx_size += 4;

		coinbase_append = true;
	}
	if (coinbase_append) {
		unsigned char xsig[100];
		int xsig_len = 0;
		if (*coinbase_sig) {
			n = (int)strlen(coinbase_sig);
			if (cbtx[41] + xsig_len + n <= 100) {
				memcpy(xsig + xsig_len, coinbase_sig, n);
				xsig_len += n;
			}
			else {
				applog(LOG_WARNING, "Signature does not fit in coinbase, skipping");
			}
		}
		tmp = json_object_get(val, "coinbaseaux");
		if (tmp && json_is_object(tmp)) {
			void *iter = json_object_iter(tmp);
			while (iter) {
				unsigned char buf[100];
				const char *s = json_string_value(json_object_iter_value(iter));
				n = s ? (int)(strlen(s) / 2) : 0;
				if (!s || n > 100 || !hex2bin(buf, s, n)) {
					applog(LOG_ERR, "JSON invalid coinbaseaux");
					break;
				}
				if (cbtx[41] + xsig_len + n <= 100) {
					memcpy(xsig + xsig_len, buf, n);
					xsig_len += n;
				}
				iter = json_object_iter_next(tmp, iter);
			}
		}
		if (xsig_len) {
			unsigned char *ssig_end = cbtx + 42 + cbtx[41];
			int push_len = cbtx[41] + xsig_len < 76 ? 1 :
				cbtx[41] + 2 + xsig_len > 100 ? 0 : 2;
			n = xsig_len + push_len;
			memmove(ssig_end + n, ssig_end, cbtx_size - 42 - cbtx[41]);
			cbtx[41] += n;
			if (push_len == 2)
				*(ssig_end++) = 0x4c; 
			if (push_len)
				*(ssig_end++) = xsig_len;
			memcpy(ssig_end, xsig, xsig_len);
			cbtx_size += n;
		}
	}

	n = varint_encode(txc_vi, 1 + tx_count);
	work->txs = (char*)malloc(2 * (n + cbtx_size + tx_size) + 1);
	dbin2hex(work->txs, txc_vi, n);
	dbin2hex(work->txs + 2 * n, cbtx, cbtx_size);

	// generate merkle root 
	merkle_tree = (uchar(*)[32]) calloc(((1 + tx_count + 1) & ~1), 32);




	sha256d(merkle_tree[0], cbtx, cbtx_size);


	for (i = 0; i < tx_count; i++) {
		tmp = json_array_get(txa, i);
		const char *tx_hex = json_string_value(json_object_get(tmp, "data"));
		const int tx_size = tx_hex ? (int)(strlen(tx_hex) / 2) : 0;
		unsigned char *tx = (uchar*)malloc(tx_size);
		if (!tx_hex || !hex2bin(tx, tx_hex, tx_size)) {
			applog(LOG_ERR, "JSON invalid transactions");
			free(tx);
			goto out;
		}
		sha256d(merkle_tree[1 + i], tx, tx_size);
		if (!submit_coinbase)
			strcat(work->txs, tx_hex);
	}
	n = 1 + tx_count;
	while (n > 1) {
		if (n % 2) {
			memcpy(merkle_tree[n], merkle_tree[n - 1], 32);
			++n;
		}
		n /= 2;
		for (i = 0; i < n; i++)
			sha256d(merkle_tree[i], merkle_tree[2 * i], 64);
	}

	// assemble block header 
	work->data[0] = swab32(version);
	for (i = 0; i < 8; i++)
		work->data[8 - i] = le32dec(prevhash + i);
	for (i = 0; i < 8; i++)
		work->data[9 + i] = be32dec((uint32_t *)merkle_tree[0] + i);
	work->data[17] = swab32(curtime);
	work->data[18] = le32dec(&bits);
	memset(work->data + 19, 0x00, 52);

	work->data[20] = 0x80000000;
	work->data[31] = 0x00000280;

	if (unlikely(!jobj_binary(val, "target", target, sizeof(target)))) {
		applog(LOG_ERR, "JSON invalid target");
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(work->target); i++)
		work->target[7 - i] = be32dec(target + i);

	tmp = json_object_get(val, "workid");
	if (tmp) {
		if (!json_is_string(tmp)) {
			applog(LOG_ERR, "JSON invalid workid");
			goto out;
		}
		work->workid = strdup(json_string_value(tmp));
	}

	rc = true;
out:
	// Long polling 
	tmp = json_object_get(val, "longpollid");
	if (want_longpoll && json_is_string(tmp)) {
		free(lp_id);
		lp_id = strdup(json_string_value(tmp));
		if (!have_longpoll) {
			char *lp_uri;
			tmp = json_object_get(val, "longpolluri");
			lp_uri = json_is_string(tmp) ? strdup(json_string_value(tmp)) : rpc_url;
			have_longpoll = true;
			tq_push(thr_info[longpoll_thr_id].q, lp_uri);
		}
	}

	free(merkle_tree);
	free(cbtx);
	return rc;
}


static bool gbt_work_decode_mtp(const json_t *val, struct work *work)
{

//restart_threads();
	int i, n;
	uint32_t version, curtime, bits;
	uint32_t prevhash[8];
	uint32_t target[8];
	int cbtx_size;
	uchar *cbtx = NULL;
	int32_t mtpVersion = 0x1000;

	int tx_count, tx_size;
	uchar txc_vi[9];
	uchar(*merkle_tree)[32] = NULL;
	bool coinbase_append = false;
	bool submit_coinbase = false;
	bool version_force = false;
	bool version_reduce = false;
	json_t *tmp, *txa;
	json_t *mpay;
	json_t *mnval;
	json_t *mnamount;
	json_t *mnaddy;
	json_t *mnscript;
	bool rc = false;

	tmp = json_object_get(val, "mutable");
	if (tmp && json_is_array(tmp)) {
		n = (int)json_array_size(tmp);
		for (i = 0; i < n; i++) {
			const char *s = json_string_value(json_array_get(tmp, i));
			if (!s)
				continue;
			if (!strcmp(s, "coinbase/append"))
				coinbase_append = true;
			else if (!strcmp(s, "submit/coinbase"))
				submit_coinbase = true;
			else if (!strcmp(s, "version/force"))
				version_force = true;
			else if (!strcmp(s, "version/reduce"))
				version_reduce = true;
		}
	}

	tmp = json_object_get(val, "height");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid height");
		goto out;
	}
	if (work->height == (int)json_integer_value(tmp))
							return false;

	work->height = (int)json_integer_value(tmp);
	applog(LOG_BLUE, "Current block is %d", work->height);

	tmp = json_object_get(val, "version");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid version");
		goto out;
	}
	version = (uint32_t)json_integer_value(tmp);
	if ((version & 0xffU) > BLOCK_VERSION_CURRENT) {
		if (version_reduce) {
			version = (version & ~0xffU) | BLOCK_VERSION_CURRENT;
		}
		else if (have_gbt && allow_getwork && !version_force) {
			applog(LOG_DEBUG, "Switching to getwork, gbt version %d", version);
			have_gbt = false;
			goto out;
		}
		else if (!version_force) {
			applog(LOG_ERR, "Unrecognized block version: %u", version);
			goto out;
		}
	}

	if (unlikely(!jobj_binary(val, "previousblockhash", prevhash, sizeof(prevhash)))) {
		applog(LOG_ERR, "JSON invalid previousblockhash");
		goto out;
	}

	tmp = json_object_get(val, "curtime");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid curtime");
		goto out;
	}

// random seed initialized with time and pid
#ifdef _MSC_VER 		
	srand(time(NULL) - _getpid()); 
#elif __GNUC__
	srand(time(NULL) - getpid());
#endif		
	
	uint32_t ranraw[1]; 
	ranraw[0] = (uint32_t)rand();

	curtime = (uint32_t)json_integer_value(tmp);

	if (unlikely(!jobj_binary(val, "bits", &bits, sizeof(bits)))) {
		applog(LOG_ERR, "JSON invalid bits");
		goto out;
	}

	// find count and size of transactions 
	txa = json_object_get(val, "transactions");
	if (!txa || !json_is_array(txa)) {
		applog(LOG_ERR, "JSON invalid transactions");
		goto out;
	}
	tx_count = (int)json_array_size(txa);
	tx_size = 0;
	for (i = 0; i < tx_count; i++) {
		const json_t *tx = json_array_get(txa, i);
		const char *tx_hex = json_string_value(json_object_get(tx, "data"));
		if (!tx_hex) {
			applog(LOG_ERR, "JSON invalid transactions");
			goto out;
		}
		tx_size += (int)(strlen(tx_hex) / 2);
	}

	// build coinbase transaction 
	tmp = json_object_get(val, "coinbasetxn");
	if (tmp) {
		const char *cbtx_hex = json_string_value(json_object_get(tmp, "data"));
		cbtx_size = cbtx_hex ? (int)strlen(cbtx_hex) / 2 : 0;
		cbtx = (uchar*)malloc(cbtx_size + 100);
		if (cbtx_size < 60 || !hex2bin(cbtx, cbtx_hex, cbtx_size)) {
			applog(LOG_ERR, "JSON invalid coinbasetxn");
			goto out;
		}
	}
	else {
		int64_t cbvalue;
		if (!pk_script_size) {
			if (allow_getwork) {
				applog(LOG_INFO, "No payout address provided, switching to getwork");
				have_gbt = false;
			}
			else
				applog(LOG_ERR, "No payout address provided");
			goto out;
		}
		tmp = json_object_get(val, "coinbasevalue");
		if (!tmp || !json_is_number(tmp)) {
			applog(LOG_ERR, "JSON invalid coinbasevalue");
			goto out;
		}
		mpay = json_object_get(val,"znode_payments_started");
//		mpay = false;
		if (mpay) {
		mnval = json_object_get(val, "znode");
			mnamount = json_object_get(mnval,"amount");
			mnaddy = json_object_get(mnval, "payee");
			mnscript = json_object_get(mnval, "script");
/*
		printf("mn addy %s", json_string_value(mnaddy));
		printf("mn amount %d", json_integer_value(mnamount));
		printf("mn script %d", json_string_value(mnscript));
*/
		}

		cbvalue = (int64_t)(json_is_integer(tmp) ? json_integer_value(tmp) : json_number_value(tmp));
		cbtx = (uchar*)malloc(256*256);
		le32enc((uint32_t *)cbtx, 1); // version /
		cbtx[4] = 1; // in-counter /
		memset(cbtx + 5, 0x00, 32); // prev txout hash /
		le32enc((uint32_t *)(cbtx + 37), 0xffffffff); // prev txout index /
		cbtx_size = 43;
		// BIP 34: height in coinbase /
//		for (n = work->height; n; n >>= 8)
//			cbtx[cbtx_size++] = n & 0xff;
		cbtx[cbtx_size++] = 04;
		for (int i=0;i<4;i++)
		cbtx[cbtx_size++] = ((unsigned char*)ranraw)[i];
		cbtx[42] = cbtx_size - 43;
		cbtx[41] = cbtx_size - 42; // scriptsig length /
		le32enc((uint32_t *)(cbtx + cbtx_size), 0xffffffff); // sequence /
		cbtx_size += 4;
		
		cbtx[cbtx_size++] = (mpay && json_integer_value(mnamount)!=0)? 7:6; // out-counter /

		le32enc((uint32_t *)(cbtx + cbtx_size), (uint32_t)cbvalue); // value /
		le32enc((uint32_t *)(cbtx + cbtx_size + 4), cbvalue >> 32);
		cbtx_size += 8;
		cbtx[cbtx_size++] = (uint8_t)pk_script_size; // txout-script length /
		memcpy(cbtx + cbtx_size, pk_script, pk_script_size);
		cbtx_size += (int)pk_script_size;
////// null data
//		le32enc((uint32_t *)(cbtx + cbtx_size), (uint32_t)0); // value /
//		le32enc((uint32_t *)(cbtx + cbtx_size + 4), 0 >> 32);
//		cbtx_size += 8;
//		cbtx[cbtx_size++] = (uint8_t)pk_null_size; // txout-script length /
//		memcpy(cbtx + cbtx_size, pk_null, pk_null_size);
//		cbtx_size += (int)pk_null_size;

		/// append here dev fee and masternode payment ////
		/*   test */
		char coinb0[90] = { 0 };
		char coinb1[74] = { 0 };
		char coinb2[74] = { 0 };
		char coinb3[74] = { 0 };
		char coinb4[74] = { 0 };
		char coinb5[74] = { 0 };
		char coinb6[74] = { 0 };
		char coinb7[90] = { 0 };
		char script_payee[1024];


		  // for mainnet

		base58_decode("aCAgTPgtYcA4EysU4UKC86EQd5cTtHtCcr", script_payee);
		job_pack_tx(coinb1, 50000000, script_payee);

		base58_decode("aHu897ivzmeFuLNB6956X6gyGeVNHUBRgD", script_payee);
		job_pack_tx(coinb2, 50000000, script_payee);

		base58_decode("aQ18FBVFtnueucZKeVg4srhmzbpAeb1KoN", script_payee);
		job_pack_tx(coinb3, 50000000, script_payee);

		base58_decode("a1HwTdCmQV3NspP2QqCGpehoFpi8NY4Zg3", script_payee);
		job_pack_tx(coinb4, 150000000, script_payee);

		base58_decode("a1kCCGddf5pMXSipLVD9hBG2MGGVNaJ15U", script_payee);
		job_pack_tx(coinb5, 50000000, script_payee);

/*		
		// for testnet with znode payment
		base58_decode("TDk19wPKYq91i18qmY6U9FeTdTxwPeSveo", script_payee);
		job_pack_tx(coinb1, 50000000, script_payee);

		base58_decode("TWZZcDGkNixTAMtRBqzZkkMHbq1G6vUTk5", script_payee);
		job_pack_tx(coinb2, 50000000, script_payee);

		base58_decode("TRZTFdNCKCKbLMQV8cZDkQN9Vwuuq4gDzT", script_payee);
		job_pack_tx(coinb3, 50000000, script_payee);

		base58_decode("TG2ruj59E5b1u9G3F7HQVs6pCcVDBxrQve", script_payee);
		job_pack_tx(coinb4, 150000000, script_payee);

		base58_decode("TCsTzQZKVn4fao8jDmB9zQBk9YQNEZ3XfS", script_payee);
		job_pack_tx(coinb5, 50000000, script_payee);
*/		
	
		if (mpay && json_integer_value(mnamount)!=0) {
		base58_decode((char*)json_string_value(mnaddy), script_payee);
//		memcpy(script_payee, json_string_value(mnscript),strlen(json_string_value(mnscript)));
		job_pack_tx(coinb6, json_integer_value(mnamount), script_payee);
		}

/*
		"payee": "TQd92H738k7QpEpYPrEFrHDoawsJjW4XhT",
			"script" : "76a914a0be3efc7cec6597a0c1c01037fc19e99c7b707f88ac",
			"amount" : 750000000
*/

		strcat(coinb7, "00000000"); // locktime

		hex2bin(cbtx + cbtx_size, coinb1, strlen(coinb1));
		cbtx_size = cbtx_size + (int)((strlen(coinb1)) / 2);

		hex2bin(cbtx + cbtx_size, coinb2, strlen(coinb2));
		cbtx_size = cbtx_size + (int)((strlen(coinb2)) / 2);

		hex2bin(cbtx + cbtx_size, coinb3, strlen(coinb3));
		cbtx_size = cbtx_size + (int)((strlen(coinb3)) / 2);

		hex2bin(cbtx + cbtx_size, coinb4, strlen(coinb4));
		cbtx_size = cbtx_size + (int)((strlen(coinb4)) / 2);

		hex2bin(cbtx + cbtx_size, coinb5, strlen(coinb5));
		cbtx_size = cbtx_size + (int)((strlen(coinb5)) / 2);

		hex2bin(cbtx + cbtx_size, coinb6, strlen(coinb6));
		cbtx_size = cbtx_size + (int)((strlen(coinb6)) / 2);
		
		hex2bin(cbtx + cbtx_size, coinb7, strlen(coinb7));
		cbtx_size = cbtx_size + (int)((strlen(coinb7)) / 2);
		coinbase_append = true;
	}
	if (coinbase_append) {
		unsigned char xsig[100];
		int xsig_len = 0;
		if (*coinbase_sig) {
			n = (int)strlen(coinbase_sig);
			if (cbtx[41] + xsig_len + n <= 100) {
				memcpy(xsig + xsig_len, coinbase_sig, n);
				xsig_len += n;
			}
			else {
				applog(LOG_WARNING, "Signature does not fit in coinbase, skipping");
			}
		}
		tmp = json_object_get(val, "coinbaseaux");
		if (tmp && json_is_object(tmp)) {
			void *iter = json_object_iter(tmp);
			while (iter) {
				unsigned char buf[100];
				const char *s = json_string_value(json_object_iter_value(iter));
				n = s ? (int)(strlen(s) / 2) : 0;
				if (!s || n > 100 || !hex2bin(buf, s, n)) {
					applog(LOG_ERR, "JSON invalid coinbaseaux");
					break;
				}
				if (cbtx[41] + xsig_len + n <= 100) {
					memcpy(xsig + xsig_len, buf, n);
					xsig_len += n;
				}
				iter = json_object_iter_next(tmp, iter);
			}
		}
		if (xsig_len) {
			unsigned char *ssig_end = cbtx + 42 + cbtx[41];
			int push_len = cbtx[41] + xsig_len < 76 ? 1 :
				cbtx[41] + 2 + xsig_len > 100 ? 0 : 2;
			n = xsig_len + push_len;
			memmove(ssig_end + n, ssig_end, cbtx_size - 42 - cbtx[41]);
			cbtx[41] += n;
			if (push_len == 2)
				*(ssig_end++) = 0x4c;
			if (push_len)
				*(ssig_end++) = xsig_len;
			memcpy(ssig_end, xsig, xsig_len);
			cbtx_size += n;
		}
	}

	n = varint_encode(txc_vi, 1 + tx_count);
	work->txs = (char*)malloc(2 * (n + cbtx_size + tx_size) + 1);
	dbin2hex(work->txs, txc_vi, n);
	dbin2hex(work->txs + 2 * n, cbtx, cbtx_size);

	// generate merkle root 
	merkle_tree = (uchar(*)[32]) calloc(((1 + tx_count + 1) & ~1), 32);

	sha256d(merkle_tree[0], cbtx, cbtx_size);

	for (i = 0; i < tx_count; i++) {
		tmp = json_array_get(txa, i);
		const char *tx_hex = json_string_value(json_object_get(tmp, "data"));
		const int tx_size = tx_hex ? (int)(strlen(tx_hex) / 2) : 0;
		unsigned char *tx = (uchar*)malloc(tx_size);
		if (!tx_hex || !hex2bin(tx, tx_hex, tx_size)) {
			applog(LOG_ERR, "JSON invalid transactions");
			free(tx);
			goto out;
		}
		sha256d(merkle_tree[1 + i], tx, tx_size);
		if (!submit_coinbase)
			strcat(work->txs, tx_hex);
	}
	n = 1 + tx_count;
	while (n > 1) {
		if (n % 2) {
			memcpy(merkle_tree[n], merkle_tree[n - 1], 32);
			++n;
		}
		n /= 2;
		for (i = 0; i < n; i++)
			sha256d(merkle_tree[i], merkle_tree[2 * i], 64);
	}

	// assemble block header 
	work->data[0] = (version);
	for (i = 0; i < 8; i++)
		work->data[8 - i] = be32dec(prevhash + i);
	for (i = 0; i < 8; i++)
		work->data[9 + i] = le32dec((uint32_t *)merkle_tree[0] + i);
	work->data[17] = (curtime);
	work->data[18] = be32dec(&bits);
	memset(work->data + 19, 0x00, 52);
/************************************************************************/
//mtp stuff

	work->data[20] = (mtpVersion);
/*************************************************************************/
	
//	work->data[20] = 0x80000000;
//	work->data[20] = 0x80000000;
//	work->data[31] = 0x000002A0;

	if (unlikely(!jobj_binary(val, "target", target, sizeof(target)))) {
		applog(LOG_ERR, "JSON invalid target");
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(work->target); i++)
		work->target[7 - i] = be32dec(target + i);

		work->targetdiff = target_to_diff(work->target);

	tmp = json_object_get(val, "workid");
	if (tmp) {
		if (!json_is_string(tmp)) {
			applog(LOG_ERR, "JSON invalid workid");
			goto out;
		}
		work->workid = strdup(json_string_value(tmp));
	}

	rc = true;
out:
	// Long polling 
	tmp = json_object_get(val, "longpollid");
	if (want_longpoll && json_is_string(tmp)) {
		free(lp_id);
		lp_id = strdup(json_string_value(tmp));
		if (!have_longpoll) {
			char *lp_uri;
			tmp = json_object_get(val, "longpolluri");
			lp_uri = json_is_string(tmp) ? strdup(json_string_value(tmp)) : rpc_url;
			have_longpoll = true;
			tq_push(thr_info[longpoll_thr_id].q, lp_uri);
		}
	}

	free(merkle_tree);
	free(cbtx);
	return rc;
}


static bool gbt_work_decode_mtptcr(const json_t *val, struct work *work)
{

	//restart_threads();
	int i, n;
	uint32_t version, curtime, bits;
	uint32_t prevhash[8];
	uint32_t target[8];
	int64_t coinbasesubsidy;
	int64_t devf1;
	int64_t devf2;
	int64_t devf3;
	int64_t devf4;
	int cbtx_size;
	uchar *cbtx = NULL;
	int32_t mtpVersion = 0x1000;
	char* coinbase_payload;
	const int rewardsStage2Start = 71000;
	const int rewardsStage3Start = 840000;
	const int rewardsStage4Start = 1680000;
	const int rewardsStage5Start = 2520000;
	const int rewardsStage6Start = 3366000;
	const int64_t devfi = 500000;
	int tx_count, tx_size;
	uchar txc_vi[9];
	uchar(*merkle_tree)[32] = NULL;
	bool coinbase_append = false;
	bool submit_coinbase = false;
	bool version_force = false;
	bool version_reduce = false;
	json_t *tmp, *txa;
	json_t *subsidy;

	json_t *mnval;
	json_t *mnamount;
	json_t *mnaddy;
	json_t *mnscript;
	bool rc = false;
	json_t* myobj = json_object_get(val, "coinbase_payload");
	int myobj_len = (int)strlen(json_string_value(myobj));
	coinbase_payload = (char*)malloc(myobj_len);
	coinbase_payload = (char*)json_string_value(myobj);
	tmp = json_object_get(val, "mutable");
	if (tmp && json_is_array(tmp)) {
		n = (int)json_array_size(tmp);
		for (i = 0; i < n; i++) {
			const char *s = json_string_value(json_array_get(tmp, i));
			if (!s)
				continue;
			if (!strcmp(s, "coinbase/append"))
				coinbase_append = true;
			else if (!strcmp(s, "submit/coinbase"))
				submit_coinbase = true;
			else if (!strcmp(s, "version/force"))
				version_force = true;
			else if (!strcmp(s, "version/reduce"))
				version_reduce = true;
		}
	}

	tmp = json_object_get(val, "height");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid height");
		goto out;
	}
	if (work->height == (int)json_integer_value(tmp))
		return false;

	work->height = (int)json_integer_value(tmp);
	applog(LOG_BLUE, "Current block is %d", work->height);

	tmp = json_object_get(val, "version");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid version");
		goto out;
	}
	version = (uint32_t)json_integer_value(tmp);
	if ((version & 0xffU) > BLOCK_VERSION_CURRENT) {
		if (version_reduce) {
			version = (version & ~0xffU) | BLOCK_VERSION_CURRENT;
		}
		else if (have_gbt && allow_getwork && !version_force) {
			applog(LOG_DEBUG, "Switching to getwork, gbt version %d", version);
			have_gbt = false;
			goto out;
		}
		else if (!version_force) {
			applog(LOG_ERR, "Unrecognized block version: %u", version);
			goto out;
		}
	}

	if (unlikely(!jobj_binary(val, "previousblockhash", prevhash, sizeof(prevhash)))) {
		applog(LOG_ERR, "JSON invalid previousblockhash");
		goto out;
	}

	tmp = json_object_get(val, "curtime");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid curtime");
		goto out;
	}

	// random seed initialized with time and pid
#ifdef _MSC_VER 		
	srand(time(NULL) - _getpid());
#elif __GNUC__
	srand(time(NULL) - getpid());
#endif		

	uint32_t ranraw[1];
	ranraw[0] = (uint32_t)rand();

	curtime = (uint32_t)json_integer_value(tmp);

	if (unlikely(!jobj_binary(val, "bits", &bits, sizeof(bits)))) {
		applog(LOG_ERR, "JSON invalid bits");
		goto out;
	}

	// find count and size of transactions 
	txa = json_object_get(val, "transactions");
	if (!txa || !json_is_array(txa)) {
		applog(LOG_ERR, "JSON invalid transactions");
		goto out;
	}
	tx_count = (int)json_array_size(txa);
	tx_size = 0;
	for (i = 0; i < tx_count; i++) {
		const json_t *tx = json_array_get(txa, i);
		const char *tx_hex = json_string_value(json_object_get(tx, "data"));
		if (!tx_hex) {
			applog(LOG_ERR, "JSON invalid transactions");
			goto out;
		}
		tx_size += (int)(strlen(tx_hex) / 2);
	}

	// build coinbase transaction 
	tmp = json_object_get(val, "coinbasetxn");
	if (tmp) {
		const char *cbtx_hex = json_string_value(json_object_get(tmp, "data"));
		cbtx_size = cbtx_hex ? (int)strlen(cbtx_hex) / 2 : 0;
		cbtx = (uchar*)malloc(cbtx_size + 100);
		if (cbtx_size < 60 || !hex2bin(cbtx, cbtx_hex, cbtx_size)) {
			applog(LOG_ERR, "JSON invalid coinbasetxn");
			goto out;
		}
	}
	else {
		int64_t cbvalue;
		if (!pk_script_size) {
			if (allow_getwork) {
				applog(LOG_INFO, "No payout address provided, switching to getwork");
				have_gbt = false;
			}
			else
				applog(LOG_ERR, "No payout address provided");
			goto out;
		}
		tmp = json_object_get(val, "coinbasevalue");
		subsidy = json_object_get(val, "coinbasesubsidy");
		coinbasesubsidy = (int64_t)(json_is_integer(subsidy) ? json_integer_value(subsidy) : json_number_value(subsidy));
		if (work->height < rewardsStage3Start) {  // 15% tnode  miner 1%
			devf1 = (coinbasesubsidy * 10) / 100;
			devf2 = (coinbasesubsidy * 64) / 100;
			devf3 = (coinbasesubsidy * 10) / 100;
			devf4 = devfi;
		}
		else if (work->height < rewardsStage4Start) { // 15% tnode ; miner 5%
			devf1 = (coinbasesubsidy * 10) / 100;
			devf2 = (coinbasesubsidy * 55) / 100;
			devf3 = (coinbasesubsidy * 15) / 100;
			devf4 = 5 * devfi;
		}
		else if (work->height < rewardsStage5Start) { // 20% tnode; miner 10%
			devf1 = (coinbasesubsidy * 10) / 100;
			devf2 = (coinbasesubsidy * 40) / 100;
			devf3 = (coinbasesubsidy * 20) / 100;
			devf4 = 10 * devfi;
		}
		else if (work->height < rewardsStage6Start) { // 20% tnode; miner 15%
			devf1 = (coinbasesubsidy * 10) / 100;
			devf2 = (coinbasesubsidy * 35) / 100;
			devf3 = (coinbasesubsidy * 20) / 100;
			devf4 = 15 * devfi;
		}
		else {										  // 25% tnode; miner 20%
			devf1 = (coinbasesubsidy * 10) / 100;
			devf2 = (coinbasesubsidy * 20) / 100;
			devf3 = (coinbasesubsidy * 25) / 100;
			devf4 = 20 * devfi;
		}
		if (!tmp || !json_is_number(tmp)) {
			applog(LOG_ERR, "JSON invalid coinbasevalue");
			goto out;
		}

		mnval = json_object_get(val, "tnode");
		mnamount = json_object_get(mnval, "amount");
		mnaddy = json_object_get(mnval, "payee");
		mnscript = json_object_get(mnval, "script");
		/*
		printf("mn addy %s", json_string_value(mnaddy));
		printf("mn amount %d", json_integer_value(mnamount));
		printf("mn script %d", json_string_value(mnscript));
		*/

		cbvalue = (int64_t)(json_is_integer(tmp) ? json_integer_value(tmp) : json_number_value(tmp));
		cbvalue = (uint32_t)cbvalue - (uint32_t)devf4;
		cbtx = (uchar*)malloc(256 * 256);
//		le32enc((uint32_t *)cbtx, 1); // version /
		be32enc((uint32_t*)cbtx, 0x03000500); // version from tecra wallet 1.7
		cbtx[4] = 1; // in-counter /
		memset(cbtx + 5, 0x00, 32); // prev txout hash /
		le32enc((uint32_t *)(cbtx + 37), 0xffffffff); // prev txout index /
		cbtx_size = 43;
		// BIP 34: height in coinbase /
		//		for (n = work->height; n; n >>= 8)
		//			cbtx[cbtx_size++] = n & 0xff;
		cbtx[cbtx_size++] = 04;
		for (int i = 0; i<4; i++)
			cbtx[cbtx_size++] = ((unsigned char*)ranraw)[i];
		cbtx[42] = cbtx_size - 43;
		cbtx[41] = cbtx_size - 42; // scriptsig length /
		le32enc((uint32_t *)(cbtx + cbtx_size), 0xffffffff); // sequence /
		cbtx_size += 4;

		cbtx[cbtx_size++] = 6; // out-counter /

		le32enc((uint32_t *)(cbtx + cbtx_size), (uint32_t)cbvalue); // value /
		le32enc((uint32_t *)(cbtx + cbtx_size + 4), cbvalue >> 32);
		cbtx_size += 8;
		cbtx[cbtx_size++] = (uint8_t)pk_script_size; // txout-script length /
		memcpy(cbtx + cbtx_size, pk_script, pk_script_size);
		cbtx_size += (int)pk_script_size;
		////// null data
		//		le32enc((uint32_t *)(cbtx + cbtx_size), (uint32_t)0); // value /
		//		le32enc((uint32_t *)(cbtx + cbtx_size + 4), 0 >> 32);
		//		cbtx_size += 8;
		//		cbtx[cbtx_size++] = (uint8_t)pk_null_size; // txout-script length /
		//		memcpy(cbtx + cbtx_size, pk_null, pk_null_size);
		//		cbtx_size += (int)pk_null_size;

		/// append here dev fee and masternode payment ////
		/*   test */
		char coinb0[90] = { 0 };
		char coinb1[74] = { 0 };
		char coinb2[74] = { 0 };
		char coinb3[74] = { 0 };
		char coinb4[74] = { 0 };
		char coinb5[74] = { 0 };
		char coinb6[74] = { 0 };
		char coinb7[90] = { 0 };
		char script_payee[1024];


		// for mainnet

		base58_decode("TC4frBMpSm2PF2FuUNqJ3qicn4EHL59ejL", script_payee);
		job_pack_tx(coinb1, devf1, script_payee);

		base58_decode("TNTkzXXJf8Yw3W1i29iQQgcxVfc3JicS2s", script_payee);
		job_pack_tx(coinb2, devf2, script_payee);

		base58_decode("TD6A1JC3jUT91riUxpQpMQZJVBa4xU2vQC", script_payee);
		job_pack_tx(coinb3, devf3, script_payee);

		base58_decode("TLddkwY6hmSpB2y8aBNyDkDDDh9ohhaoRG", script_payee);
		job_pack_tx(coinb4, devf4, script_payee);

		if (json_integer_value(mnamount) != 0) {
			base58_decode((char*)json_string_value(mnaddy), script_payee);
			//		memcpy(script_payee, json_string_value(mnscript),strlen(json_string_value(mnscript)));
			job_pack_tx(coinb6, json_integer_value(mnamount), script_payee);
		}

		strcat(coinb7, "00000000"); // locktime

		hex2bin(cbtx + cbtx_size, coinb1, strlen(coinb1));
		cbtx_size = cbtx_size + (int)((strlen(coinb1)) / 2);

		hex2bin(cbtx + cbtx_size, coinb2, strlen(coinb2));
		cbtx_size = cbtx_size + (int)((strlen(coinb2)) / 2);

		hex2bin(cbtx + cbtx_size, coinb3, strlen(coinb3));
		cbtx_size = cbtx_size + (int)((strlen(coinb3)) / 2);

		hex2bin(cbtx + cbtx_size, coinb4, strlen(coinb4));
		cbtx_size = cbtx_size + (int)((strlen(coinb4)) / 2);

		//		hex2bin(cbtx + cbtx_size, coinb5, strlen(coinb5));
		//		cbtx_size = cbtx_size + (int)((strlen(coinb5)) / 2);

		hex2bin(cbtx + cbtx_size, coinb6, strlen(coinb6));
		cbtx_size = cbtx_size + (int)((strlen(coinb6)) / 2);

		hex2bin(cbtx + cbtx_size, coinb7, strlen(coinb7));
		cbtx_size = cbtx_size + (int)((strlen(coinb7)) / 2);

		hex2bin(cbtx + cbtx_size, coinbase_payload, myobj_len);
		cbtx_size = cbtx_size + (int)(myobj_len / 2);
		free(coinbase_payload);

		coinbase_append = true;
	}
	if (coinbase_append) {
		unsigned char xsig[100];
		int xsig_len = 0;
		if (*coinbase_sig) {
			n = (int)strlen(coinbase_sig);
			if (cbtx[41] + xsig_len + n <= 100) {
				memcpy(xsig + xsig_len, coinbase_sig, n);
				xsig_len += n;
			}
			else {
				applog(LOG_WARNING, "Signature does not fit in coinbase, skipping");
			}
		}
		tmp = json_object_get(val, "coinbaseaux");
		if (tmp && json_is_object(tmp)) {
			void *iter = json_object_iter(tmp);
			while (iter) {
				unsigned char buf[100];
				const char *s = json_string_value(json_object_iter_value(iter));
				n = s ? (int)(strlen(s) / 2) : 0;
				if (!s || n > 100 || !hex2bin(buf, s, n)) {
					applog(LOG_ERR, "JSON invalid coinbaseaux");
					break;
				}
				if (cbtx[41] + xsig_len + n <= 100) {
					memcpy(xsig + xsig_len, buf, n);
					xsig_len += n;
				}
				iter = json_object_iter_next(tmp, iter);
			}
		}
		if (xsig_len) {
			unsigned char *ssig_end = cbtx + 42 + cbtx[41];
			int push_len = cbtx[41] + xsig_len < 76 ? 1 :
				cbtx[41] + 2 + xsig_len > 100 ? 0 : 2;
			n = xsig_len + push_len;
			memmove(ssig_end + n, ssig_end, cbtx_size - 42 - cbtx[41]);
			cbtx[41] += n;
			if (push_len == 2)
				*(ssig_end++) = 0x4c;
			if (push_len)
				*(ssig_end++) = xsig_len;
			memcpy(ssig_end, xsig, xsig_len);
			cbtx_size += n;
		}
	}
	printf("cbtx %s \n", cbtx);
	n = varint_encode(txc_vi, 1 + tx_count);
	work->txs = (char*)malloc(2 * (n + cbtx_size + tx_size) + 1);
	dbin2hex(work->txs, txc_vi, n);
	dbin2hex(work->txs + 2 * n, cbtx, cbtx_size);

	// generate merkle root 
	merkle_tree = (uchar(*)[32]) calloc(((1 + tx_count + 1) & ~1), 32);

	sha256d(merkle_tree[0], cbtx, cbtx_size);

	for (i = 0; i < tx_count; i++) {
		tmp = json_array_get(txa, i);
		const char *tx_hex = json_string_value(json_object_get(tmp, "data"));
		const int tx_size = tx_hex ? (int)(strlen(tx_hex) / 2) : 0;
		unsigned char *tx = (uchar*)malloc(tx_size);
		if (!tx_hex || !hex2bin(tx, tx_hex, tx_size)) {
			applog(LOG_ERR, "JSON invalid transactions");
			free(tx);
			goto out;
		}
		sha256d(merkle_tree[1 + i], tx, tx_size);
		if (!submit_coinbase)
			strcat(work->txs, tx_hex);
	}
	n = 1 + tx_count;
	while (n > 1) {
		if (n % 2) {
			memcpy(merkle_tree[n], merkle_tree[n - 1], 32);
			++n;
		}
		n /= 2;
		for (i = 0; i < n; i++)
			sha256d(merkle_tree[i], merkle_tree[2 * i], 64);
	}

	// assemble block header 
	work->data[0] = (version);
	for (i = 0; i < 8; i++)
		work->data[8 - i] = be32dec(prevhash + i);
	for (i = 0; i < 8; i++)
		work->data[9 + i] = le32dec((uint32_t *)merkle_tree[0] + i);
	work->data[17] = (curtime);
	work->data[18] = be32dec(&bits);
	memset(work->data + 19, 0x00, 52);
	/************************************************************************/
	//mtp stuff

	work->data[20] = (mtpVersion);
	/*************************************************************************/

	//	work->data[20] = 0x80000000;
	//	work->data[20] = 0x80000000;
	//	work->data[31] = 0x000002A0;

	if (unlikely(!jobj_binary(val, "target", target, sizeof(target)))) {
		applog(LOG_ERR, "JSON invalid target");
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(work->target); i++)
		work->target[7 - i] = be32dec(target + i);

	work->targetdiff = target_to_diff(work->target);

	tmp = json_object_get(val, "workid");
	if (tmp) {
		if (!json_is_string(tmp)) {
			applog(LOG_ERR, "JSON invalid workid");
			goto out;
		}
		work->workid = strdup(json_string_value(tmp));
	}

	rc = true;
out:
	// Long polling 
	tmp = json_object_get(val, "longpollid");
	if (want_longpoll && json_is_string(tmp)) {
		free(lp_id);
		lp_id = strdup(json_string_value(tmp));
		if (!have_longpoll) {
			char *lp_uri;
			tmp = json_object_get(val, "longpolluri");
			lp_uri = json_is_string(tmp) ? strdup(json_string_value(tmp)) : rpc_url;
			have_longpoll = true;
			tq_push(thr_info[longpoll_thr_id].q, lp_uri);
		}
	}

	free(merkle_tree);
	free(cbtx);
	return rc;
}



static bool gbt_work_decode_mtptcr_testnet(const json_t *val, struct work *work)
{
//printf("******************* gbt_work_decode_mtptcr **********************\n");
	//restart_threads();
	int i, n;
	uint32_t version, curtime, bits;
	uint32_t prevhash[8];
	uint32_t target[8];
	int64_t coinbasesubsidy;
	int64_t devf1;
	int64_t devf2;
	int64_t devf3;
	int64_t devf4;
	int cbtx_size;
	uchar *cbtx = NULL;
	int32_t mtpVersion = 0x1000;
	char* coinbase_payload;
	const int rewardsStage2Start = 71000;
	const int rewardsStage3Start = 840000;
	const int rewardsStage4Start = 1680000;
	const int rewardsStage5Start = 2520000;
	const int rewardsStage6Start = 3366000;

	const int64_t devfi = 500000;
	int tx_count, tx_size;
	uchar txc_vi[9];
	uchar(*merkle_tree)[32] = NULL;
	bool coinbase_append = false;
	bool submit_coinbase = false;
	bool version_force = false;
	bool version_reduce = false;
	json_t *tmp, *txa;
	json_t *subsidy;

	json_t *mnval;
	json_t *mnamount;
	json_t *mnaddy;
	json_t *mnscript;
	bool rc = false;
	json_t* myobj = json_object_get(val, "coinbase_payload");
	int myobj_len = (int)strlen(json_string_value(myobj));
	coinbase_payload = (char*)malloc(myobj_len);
	coinbase_payload = (char*)json_string_value(myobj);
//	printf("json coinbase value %s\n", json_string_value(myobj));

//	printf("coinbase_payload length %08x\n",myobj_len/2);
//	printf("coinbase_payload %s\n", coinbase_payload);

	tmp = json_object_get(val, "mutable");
	if (tmp && json_is_array(tmp)) {
		n = (int)json_array_size(tmp);
		for (i = 0; i < n; i++) {
			const char *s = json_string_value(json_array_get(tmp, i));
			if (!s)
				continue;
			if (!strcmp(s, "coinbase/append"))
				coinbase_append = true;
			else if (!strcmp(s, "submit/coinbase"))
				submit_coinbase = true;
			else if (!strcmp(s, "version/force"))
				version_force = true;
			else if (!strcmp(s, "version/reduce"))
				version_reduce = true;
		}
	}

	tmp = json_object_get(val, "height");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid height");
		goto out;
	}
	if (work->height == (int)json_integer_value(tmp))
		return false;

	work->height = (int)json_integer_value(tmp);
	applog(LOG_BLUE, "Current block is %d", work->height);

	tmp = json_object_get(val, "version");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid version");
		goto out;
	}
	version = (uint32_t)json_integer_value(tmp);

//    printf("version %08x versionff %08x\n",version, version & 0xffU);
	if ((version & 0xffU) > BLOCK_VERSION_CURRENT) {
		if (version_reduce) {
			version = (version & ~0xffU) | BLOCK_VERSION_CURRENT;
		}
		else if (have_gbt && allow_getwork && !version_force) {
			applog(LOG_DEBUG, "Switching to getwork, gbt version %d", version);
			have_gbt = false;
			goto out;
		}
		else if (!version_force) {
			applog(LOG_ERR, "Unrecognized block version: %u", version);
			goto out;
		}
	}

	if (unlikely(!jobj_binary(val, "previousblockhash", prevhash, sizeof(prevhash)))) {
		applog(LOG_ERR, "JSON invalid previousblockhash");
		goto out;
	}

	tmp = json_object_get(val, "curtime");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid curtime");
		goto out;
	}

	// random seed initialized with time and pid
#ifdef _MSC_VER 		
	srand(time(NULL) - _getpid());
#elif __GNUC__
	srand(time(NULL) - getpid());
#endif		

	uint32_t ranraw[1];
	ranraw[0] = (uint32_t)rand();

	curtime = (uint32_t)json_integer_value(tmp);

	if (unlikely(!jobj_binary(val, "bits", &bits, sizeof(bits)))) {
		applog(LOG_ERR, "JSON invalid bits");
		goto out;
	}

	// find count and size of transactions 
	txa = json_object_get(val, "transactions");
	if (!txa || !json_is_array(txa)) {
		applog(LOG_ERR, "JSON invalid transactions");
		goto out;
	}
	tx_count = (int)json_array_size(txa);
	tx_size = 0;
	for (i = 0; i < tx_count; i++) {
		const json_t *tx = json_array_get(txa, i);
		const char *tx_hex = json_string_value(json_object_get(tx, "data"));
		if (!tx_hex) {
			applog(LOG_ERR, "JSON invalid transactions");
			goto out;
		}
		tx_size += (int)(strlen(tx_hex) / 2);
	}

	// build coinbase transaction 
	tmp = json_object_get(val, "coinbasetxn");
	if (tmp) {
		const char *cbtx_hex = json_string_value(json_object_get(tmp, "data"));
		cbtx_size = cbtx_hex ? (int)strlen(cbtx_hex) / 2 : 0;
		cbtx = (uchar*)malloc(cbtx_size + 100);
		if (cbtx_size < 60 || !hex2bin(cbtx, cbtx_hex, cbtx_size)) {
			applog(LOG_ERR, "JSON invalid coinbasetxn");
			goto out;
		}
	}
	else {
		int64_t cbvalue;
		if (!pk_script_size) {
			if (allow_getwork) {
				applog(LOG_INFO, "No payout address provided, switching to getwork");
				have_gbt = false;
			}
			else
				applog(LOG_ERR, "No payout address provided");
			goto out;
		}
		tmp = json_object_get(val, "coinbasevalue");
		subsidy = json_object_get(val, "coinbasesubsidy");
		coinbasesubsidy = 11250000000; //(int64_t)(json_is_integer(subsidy) ? json_integer_value(subsidy) : json_number_value(subsidy));

		if (work->height < 600) {  // 15% tnode  miner 1%
			devf1 = (coinbasesubsidy * 10) / 100;
			devf2 = (coinbasesubsidy * 79) / 100;
			devf3 = (coinbasesubsidy * 10) / 100;
			devf4 = 0;
		}
		else
			if (work->height < rewardsStage2Start) {  // 15% tnode  miner 1%
				devf1 = (coinbasesubsidy * 10) / 100;
				devf2 = (coinbasesubsidy * 40) / 100;
				devf3 = (coinbasesubsidy * 10) / 100;
				devf4 = (coinbasesubsidy * 39) / 100;
			}
			else
		if (work->height < rewardsStage3Start) {  // 15% tnode  miner 1%
			devf1 = (coinbasesubsidy * 10) / 100;
			devf2 = (coinbasesubsidy * 64) / 100;
			devf3 = (coinbasesubsidy * 10) / 100;
			devf4 = (coinbasesubsidy * 15) / 100;
		}
		else if (work->height < rewardsStage4Start) { // 15% tnode ; miner 5%
			devf1 = (coinbasesubsidy * 10) / 100;
			devf2 = (coinbasesubsidy * 55) / 100;
			devf3 = (coinbasesubsidy * 15) / 100;
			devf4 = (coinbasesubsidy * 15) / 100;
		}
		else if (work->height < rewardsStage5Start) { // 20% tnode; miner 10%
			devf1 = (coinbasesubsidy * 10) / 100;
			devf2 = (coinbasesubsidy * 40) / 100;
			devf3 = (coinbasesubsidy * 20) / 100;
			devf4 = (coinbasesubsidy * 20) / 100;
		}
		else if (work->height < rewardsStage6Start) { // 20% tnode; miner 15%
			devf1 = (coinbasesubsidy * 10) / 100;
			devf2 = (coinbasesubsidy * 35) / 100;
			devf3 = (coinbasesubsidy * 20) / 100;
			devf4 = (coinbasesubsidy * 20) / 100;
		}
		else {										  // 25% tnode; miner 20%
			devf1 = (coinbasesubsidy * 10) / 100;
			devf2 = (coinbasesubsidy * 20) / 100;
			devf3 = (coinbasesubsidy * 25) / 100;
			devf4 = (coinbasesubsidy * 25) / 100;
		}
		if (!tmp || !json_is_number(tmp)) {
			applog(LOG_ERR, "JSON invalid coinbasevalue");
			goto out;
		}

			mnval = json_object_get(val, "tnode");
			mnamount = json_object_get(mnval, "amount");
			mnaddy = json_object_get(mnval, "payee");
			mnscript = json_object_get(mnval, "script");
		/*	
			printf("mn addy %s", json_string_value(mnaddy));
			printf("mn amount %d", json_integer_value(mnamount));
			printf("mn script %d", json_string_value(mnscript));
		*/	

		cbvalue = (int64_t)(json_is_integer(tmp) ? json_integer_value(tmp) : json_number_value(tmp));
		
		cbvalue = /*(uint32_t)*/ cbvalue + /*(uint32_t)*/devf4;
		cbtx = (uchar*)malloc(256 * 256);
		be32enc((uint32_t *)cbtx, 0x03000500); // version /
//		((uint32_t*)cbtx)[0]  = 0x03000500;
		cbtx[4] = 1; // in-counter /
		memset(cbtx + 5, 0x00, 32); // prev txout hash /
		le32enc((uint32_t *)(cbtx + 37), 0xffffffff); // prev txout index /
		cbtx_size = 43;
		// BIP 34: height in coinbase /
		//		for (n = work->height; n; n >>= 8)
		//			cbtx[cbtx_size++] = n & 0xff;
		cbtx[cbtx_size++] = 04;
		for (int i = 0; i<4; i++)
			cbtx[cbtx_size++] = ((unsigned char*)ranraw)[i];
		cbtx[42] = cbtx_size - 43;
		cbtx[41] = cbtx_size - 42; // scriptsig length /
		le32enc((uint32_t *)(cbtx + cbtx_size), 0xffffffff); // sequence /
		cbtx_size += 4;

		cbtx[cbtx_size++] = (json_integer_value(mnamount) != 0) ? 5 : 4;; // out-counter /

		le32enc((uint32_t *)(cbtx + cbtx_size), (uint32_t)cbvalue); // value /
		le32enc((uint32_t *)(cbtx + cbtx_size + 4), cbvalue >> 32);
		cbtx_size += 8;
		cbtx[cbtx_size++] = (uint8_t)pk_script_size; // txout-script length /
		memcpy(cbtx + cbtx_size, pk_script, pk_script_size);
		cbtx_size += (int)pk_script_size;
		////// null data
		//		le32enc((uint32_t *)(cbtx + cbtx_size), (uint32_t)0); // value /
		//		le32enc((uint32_t *)(cbtx + cbtx_size + 4), 0 >> 32);
		//		cbtx_size += 8;
		//		cbtx[cbtx_size++] = (uint8_t)pk_null_size; // txout-script length /
		//		memcpy(cbtx + cbtx_size, pk_null, pk_null_size);
		//		cbtx_size += (int)pk_null_size;

		/// append here dev fee and masternode payment ////
		/*   test */
		char coinb0[90] = { 0 };
		char coinb1[74] = { 0 };
		char coinb2[74] = { 0 };
		char coinb3[74] = { 0 };
		char coinb4[74] = { 0 };
		char coinb5[74] = { 0 };
		char coinb6[74] = { 0 };
		char coinb7[90] = { 0 };
		char script_payee[1024];


		// for mainnet

		base58_decode("Gf8XeYLLucQjMS8apuwBTPfbPN7eGd7r5h", script_payee);
		job_pack_tx(coinb1, devf1, script_payee);

		base58_decode("Gf3ZcqRci9yqu9ABsEp2SsvEmtvGjp6AoG", script_payee);
		job_pack_tx(coinb2, devf2, script_payee);

		base58_decode("GWrM3WGoKUegYJ6yTGHtH4ozmwZx9F8MiK", script_payee);
		job_pack_tx(coinb3, devf3, script_payee);

//		base58_decode("GJEJMcKfsk28NmkDFai6fdqB2nXfyPdPz8", script_payee);
//		job_pack_tx(coinb4, devf4, script_payee);

		if (json_integer_value(mnamount) != 0) {
		
			base58_decode((char*)json_string_value(mnaddy), script_payee);
			//		memcpy(script_payee, json_string_value(mnscript),strlen(json_string_value(mnscript)));
			job_pack_tx(coinb6, json_integer_value(mnamount), script_payee);
		}

		strcat(coinb7, "00000000"); // locktime

		hex2bin(cbtx + cbtx_size, coinb1, strlen(coinb1));
		cbtx_size = cbtx_size + (int)((strlen(coinb1)) / 2);

		hex2bin(cbtx + cbtx_size, coinb2, strlen(coinb2));
		cbtx_size = cbtx_size + (int)((strlen(coinb2)) / 2);

		hex2bin(cbtx + cbtx_size, coinb3, strlen(coinb3));
		cbtx_size = cbtx_size + (int)((strlen(coinb3)) / 2);

//		hex2bin(cbtx + cbtx_size, coinb4, strlen(coinb4));
//		cbtx_size = cbtx_size + (int)((strlen(coinb4)) / 2);

//		hex2bin(cbtx + cbtx_size, coinb5, strlen(coinb5));
//		cbtx_size = cbtx_size + (int)((strlen(coinb5)) / 2);

		if (json_integer_value(mnamount) != 0) {
		hex2bin(cbtx + cbtx_size, coinb6, strlen(coinb6));
		cbtx_size = cbtx_size + (int)((strlen(coinb6)) / 2);
		}
		hex2bin(cbtx + cbtx_size, coinb7, strlen(coinb7));
		cbtx_size = cbtx_size + (int)((strlen(coinb7)) / 2);
		cbtx[cbtx_size] = myobj_len/2;
		cbtx_size++;
		hex2bin(cbtx + cbtx_size, coinbase_payload, myobj_len);
		cbtx_size = cbtx_size + (int)(myobj_len/2);
		free(coinbase_payload);
		coinbase_append = true;
		


	}
	if (coinbase_append) {
		unsigned char xsig[100];
		int xsig_len = 0;
		if (*coinbase_sig) {
			n = (int)strlen(coinbase_sig);
			if (cbtx[41] + xsig_len + n <= 100) {
				memcpy(xsig + xsig_len, coinbase_sig, n);
				xsig_len += n;
			}
			else {
				applog(LOG_WARNING, "Signature does not fit in coinbase, skipping");
			}
		}
		tmp = json_object_get(val, "coinbaseaux");
		if (tmp && json_is_object(tmp)) {
			void *iter = json_object_iter(tmp);
			while (iter) {
				unsigned char buf[100];
				const char *s = json_string_value(json_object_iter_value(iter));
				n = s ? (int)(strlen(s) / 2) : 0;
				if (!s || n > 100 || !hex2bin(buf, s, n)) {
					applog(LOG_ERR, "JSON invalid coinbaseaux");
					break;
				}
				if (cbtx[41] + xsig_len + n <= 100) {
					memcpy(xsig + xsig_len, buf, n);
					xsig_len += n;
				}
				iter = json_object_iter_next(tmp, iter);
			}
		}
		if (xsig_len) {
			unsigned char *ssig_end = cbtx + 42 + cbtx[41];
			int push_len = cbtx[41] + xsig_len < 76 ? 1 :
				cbtx[41] + 2 + xsig_len > 100 ? 0 : 2;
			n = xsig_len + push_len;
			memmove(ssig_end + n, ssig_end, cbtx_size - 42 - cbtx[41]);
			cbtx[41] += n;
			if (push_len == 2)
				*(ssig_end++) = 0x4c;
			if (push_len)
				*(ssig_end++) = xsig_len;
			memcpy(ssig_end, xsig, xsig_len);
			cbtx_size += n;
		}
	}
//	printf("cbtx %s \n", cbtx);
	n = varint_encode(txc_vi, 1 + tx_count);
	work->txs = (char*)malloc(2 * (n + cbtx_size + tx_size) + 1);
	dbin2hex(work->txs, txc_vi, n);
	dbin2hex(work->txs + 2 * n, cbtx, cbtx_size);
//	printf("cbtx %s \n", work->txs);
	// generate merkle root 
	merkle_tree = (uchar(*)[32]) calloc(((1 + tx_count + 1) & ~1), 32);

	sha256d(merkle_tree[0], cbtx, cbtx_size);

	for (i = 0; i < tx_count; i++) {
		tmp = json_array_get(txa, i);
		const char *tx_hex = json_string_value(json_object_get(tmp, "data"));
		const int tx_size = tx_hex ? (int)(strlen(tx_hex) / 2) : 0;
		unsigned char *tx = (uchar*)malloc(tx_size);
		if (!tx_hex || !hex2bin(tx, tx_hex, tx_size)) {
			applog(LOG_ERR, "JSON invalid transactions");
			free(tx);
			goto out;
		}
		sha256d(merkle_tree[1 + i], tx, tx_size);
		if (!submit_coinbase)
			strcat(work->txs, tx_hex);
	}
	n = 1 + tx_count;
	while (n > 1) {
		if (n % 2) {
			memcpy(merkle_tree[n], merkle_tree[n - 1], 32);
			++n;
		}
		n /= 2;
		for (i = 0; i < n; i++)
			sha256d(merkle_tree[i], merkle_tree[2 * i], 64);
	}

	// assemble block header 
	work->data[0] = (version);
	for (i = 0; i < 8; i++)
		work->data[8 - i] = be32dec(prevhash + i);
	for (i = 0; i < 8; i++)
		work->data[9 + i] = le32dec((uint32_t *)merkle_tree[0] + i);
	work->data[17] = (curtime);
	work->data[18] = be32dec(&bits);
	memset(work->data + 19, 0x00, 52);
	/************************************************************************/
	//mtp stuff

	work->data[20] = (mtpVersion);
	/*************************************************************************/

	//	work->data[20] = 0x80000000;
	//	work->data[20] = 0x80000000;
	//	work->data[31] = 0x000002A0;

	if (unlikely(!jobj_binary(val, "target", target, sizeof(target)))) {
		applog(LOG_ERR, "JSON invalid target");
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(work->target); i++)
		work->target[7 - i] = be32dec(target + i);

	work->targetdiff = target_to_diff(work->target);

	tmp = json_object_get(val, "workid");
	if (tmp) {
		if (!json_is_string(tmp)) {
			applog(LOG_ERR, "JSON invalid workid");
			goto out;
		}
		work->workid = strdup(json_string_value(tmp));
	}

	rc = true;
out:
	// Long polling 
	tmp = json_object_get(val, "longpollid");
	if (want_longpoll && json_is_string(tmp)) {
		free(lp_id);
		lp_id = strdup(json_string_value(tmp));
		if (!have_longpoll) {
			char *lp_uri;
			tmp = json_object_get(val, "longpolluri");
			lp_uri = json_is_string(tmp) ? strdup(json_string_value(tmp)) : rpc_url;
			have_longpoll = true;
			tq_push(thr_info[longpoll_thr_id].q, lp_uri);
		}
	}

	free(merkle_tree);
	free(cbtx);
	return rc;
}



/* simplified method to only get some extra infos in solo mode */
static bool gbt_info(const json_t *val, struct work *work)
{
	json_t *err = json_object_get(val, "error");
	if (err && !json_is_null(err)) {
		allow_gbt = false;
		applog(LOG_INFO, "GBT not supported, block height unavailable");
		return false;
	}

	if (!work->height) {
		// complete missing data from getwork
		json_t *key = json_object_get(val, "height");
		if (key && json_is_integer(key)) {
			work->height = (uint32_t) json_integer_value(key);
			if (!opt_quiet && work->height > g_work.height) {
				if (net_diff > 0.) {
					char netinfo[64] = { 0 };
					char srate[32] = { 0 };
					sprintf(netinfo, "diff %.2f", net_diff);
					if (net_hashrate) {
						format_hashrate((double) net_hashrate, srate);
						strcat(netinfo, ", net ");
						strcat(netinfo, srate);
					}
					applog(LOG_BLUE, "%s block %d, %s",
						algo_names[opt_algo], work->height, netinfo);
				} else {
					applog(LOG_BLUE, "%s %s block %d", short_url,
						algo_names[opt_algo], work->height);
				}
				g_work.height = work->height;
			}
		}
	}

	return true;
}

#define GBT_CAPABILITIES "[\"coinbasetxn\", \"coinbasevalue\", \"longpoll\", \"workid\", \"coinbase/append\",\"coinbaseaux\"]"

static const char *gbt_req =
"{\"method\": \"getblocktemplate\", \"params\": [{\"capabilities\": "
GBT_CAPABILITIES "}], \"id\":0}\r\n";
static const char *gbt_lp_req =
"{\"method\": \"getblocktemplate\", \"params\": [{\"capabilities\": "
GBT_CAPABILITIES ", \"longpollid\": \"%s\"}], \"id\":0}\r\n";

static bool get_blocktemplate(CURL *curl, struct work *work)
{
	struct pool_infos *pool = &pools[work->pooln];
	if (!allow_gbt)
		return false;

	int curl_err = 0;
	json_t *val = json_rpc_call_pool(curl, pool, gbt_req, false, false, &curl_err);

	if (!val && curl_err == -1) {
		// when getblocktemplate is not supported, disable it
		allow_gbt = false;
		if (!opt_quiet) {
				applog(LOG_BLUE, "gbt not supported, block height notices disabled");
		}
		return false;
	}

	bool rc = gbt_info(json_object_get(val, "result"), work);

	json_decref(val);

	return rc;
}

// good alternative for wallet mining, difficulty and net hashrate
static const char *info_req =
	"{\"method\": \"getmininginfo\", \"params\": [], \"id\":8}\r\n";

static bool get_mininginfo(CURL *curl, struct work *work)
{
	struct pool_infos *pool = &pools[work->pooln];
	int curl_err = 0;

	if (have_stratum || have_longpoll || !allow_mininginfo)
		return false;

	json_t *val = json_rpc_call_pool(curl, pool, info_req, false, false, &curl_err);

	if (!val && curl_err == -1) {
		allow_mininginfo = false;
		if (opt_debug) {
				applog(LOG_DEBUG, "getmininginfo not supported");
		}
		return false;
	} else {
		json_t *res = json_object_get(val, "result");
		// "blocks": 491493 (= current work height - 1)
		// "difficulty": 0.99607860999999998
		// "networkhashps": 56475980
		// "netmhashps": 351.74414726
		if (res) {
			json_t *key = json_object_get(res, "difficulty");
			if (key) {
				if (json_is_object(key))
					key = json_object_get(key, "proof-of-work");
				if (json_is_real(key))
					net_diff = json_real_value(key);
			}
			key = json_object_get(res, "networkhashps");
			if (key && json_is_integer(key)) {
				net_hashrate = json_integer_value(key);
			}
			key = json_object_get(res, "netmhashps");
			if (key && json_is_real(key)) {
				net_hashrate = (uint64_t)(json_real_value(key) * 1e6);
			}
			key = json_object_get(res, "blocks");
			if (key && json_is_integer(key)) {
				net_blocks = json_integer_value(key);
			}
		}
	}
	json_decref(val);
	return true;
}

static const char *json_rpc_getwork =
	"{\"method\":\"getwork\",\"params\":[],\"id\":0}\r\n";

static bool get_upstream_work(CURL *curl, struct work *work)
{
	bool rc = false;
	struct timeval tv_start, tv_end, diff;
	struct pool_infos *pool = &pools[work->pooln];
	const char *rpc_req = have_gbt? gbt_req:json_rpc_getwork;
	json_t *val;
	int err;
start:
	gettimeofday(&tv_start, NULL);

	if (opt_algo == ALGO_SIA) {
		char *sia_header = sia_getheader(curl, pool);
		if (sia_header) {
			rc = sia_work_decode(sia_header, work);
			free(sia_header);
		}
		gettimeofday(&tv_end, NULL);
		if (have_stratum || unlikely(work->pooln != cur_pooln)) {
			return rc;
		}
		return rc;
	}

	if (opt_debug_threads)
		applog(LOG_DEBUG, "%s: want_longpoll=%d have_longpoll=%d",
			__func__, want_longpoll, have_longpoll);

	/* want_longpoll/have_longpoll required here to init/unlock the lp thread */
	val = json_rpc_call_pool(curl, pool, rpc_req, want_longpoll, have_longpoll, &err);
	
	gettimeofday(&tv_end, NULL);

	if (have_stratum || unlikely(work->pooln != cur_pooln)) {
		if (val)
			json_decref(val);
		return false;
	}

	if (!have_gbt && !allow_getwork) {
		applog(LOG_ERR, "No usable protocol");
		if (val)
			json_decref(val);
		return false;
	}

	if (have_gbt && allow_getwork && !val && err == CURLE_OK) {
		applog(LOG_NOTICE, "getblocktemplate failed, falling back to getwork");
		have_gbt = false;
		goto start;
	}

	if (!val)
		return false;

	if (have_gbt) {
		if (opt_algo == ALGO_MTP)
			rc = gbt_work_decode_mtp(json_object_get(val, "result"), work);
		else if (opt_algo == ALGO_MTPTCR)
			rc = gbt_work_decode_mtptcr(json_object_get(val, "result"), work);
		else 
			rc = gbt_work_decode(json_object_get(val, "result"), work);

		if (!have_gbt) {
			json_decref(val);
			goto start;
		}
	}
	else {
		rc = work_decode(json_object_get(val, "result"), work);
		}


	if (opt_protocol && rc) {
		timeval_subtract(&diff, &tv_end, &tv_start);
		/* show time because curl can be slower against versions/config */
		applog(LOG_DEBUG, "got new work in %.2f ms",
		       (1000.0 * diff.tv_sec) + (0.001 * diff.tv_usec));
	}

	json_decref(val);

	get_mininginfo(curl, work);
	get_blocktemplate(curl, work);

	return rc;
}

static void workio_cmd_free(struct workio_cmd *wc)
{
	if (!wc)
		return;

	switch (wc->cmd) {
	case WC_SUBMIT_WORK:
		aligned_free(wc->u.work);
		aligned_free(wc->t.mtp);
		break;
	default: /* do nothing */
		break;
	}

//	memset(wc, 0, sizeof(*wc));	/* poison */
	free(wc);
}

static void workio_abort()
{
	struct workio_cmd *wc;

	/* fill out work request message */
	wc = (struct workio_cmd *)calloc(1, sizeof(*wc));
	if (!wc)
		return;

	wc->cmd = WC_ABORT;

	/* send work request to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc)) {
		workio_cmd_free(wc);
	}
}

static bool workio_get_work(struct workio_cmd *wc, CURL *curl)
{
	struct work *ret_work;
	int failures = 0;

	ret_work = (struct work*)aligned_calloc(sizeof(struct work));
	if (!ret_work)
		return false;

	/* assign pool number before rpc calls */
	ret_work->pooln = wc->pooln;
	// applog(LOG_DEBUG, "%s: pool %d", __func__, wc->pooln);

	/* obtain new work from bitcoin via JSON-RPC */
	while (!get_upstream_work(curl, ret_work)) {

		if (unlikely(ret_work->pooln != cur_pooln)) {
			applog(LOG_ERR, "get_work json_rpc_call failed");
			aligned_free(ret_work);
			tq_push(wc->thr->q, NULL);
			return true;
		}

		if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
			applog(LOG_ERR, "get_work json_rpc_call failed");
			aligned_free(ret_work);
			return false;
		}

		/* pause, then restart work-request loop */
		applog(LOG_ERR, "get_work failed, retry after %d seconds",
			opt_fail_pause);
		sleep(opt_fail_pause);
	}

	/* send work to requesting thread */
	if (!tq_push(wc->thr->q, ret_work))
		aligned_free(ret_work);

	return true;
}

static bool workio_submit_work(struct workio_cmd *wc, CURL *curl)
{
	int failures = 0;
	uint32_t pooln = wc->pooln;
	// applog(LOG_DEBUG, "%s: pool %d", __func__, wc->pooln);

	/* submit solution to bitcoin via JSON-RPC */

	if (opt_algo == ALGO_MTP) {
		while (!submit_upstream_work_mtp(curl, wc->u.work, wc->t.mtp)) {
			if (pooln != cur_pooln) {
				applog(LOG_DEBUG, "work from pool %u discarded", pooln);
			
				return true;
			}
			if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
				applog(LOG_ERR, "...terminating workio thread");
				return false;
			}
			if (!opt_benchmark) {
				applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
				return false;
			}
			sleep(opt_fail_pause)	;
		}

	} else if (opt_algo == ALGO_MTPTCR) {
		while (!submit_upstream_work_mtptcr(curl, wc->u.work, wc->t.mtp)) {
			if (pooln != cur_pooln) {
				applog(LOG_DEBUG, "work from pool %u discarded", pooln);

				return true;
			}
			if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
				applog(LOG_ERR, "...terminating workio thread");
				return false;
			}
			if (!opt_benchmark) {
				applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
				return false;
			}
			sleep(opt_fail_pause);
		}

	}
	else {
		while (!submit_upstream_work(curl, wc->u.work)) {
			if (pooln != cur_pooln) {
				applog(LOG_DEBUG, "work from pool %u discarded", pooln);
				return true;
			}
			if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
				applog(LOG_ERR, "...terminating workio thread");
				return false;
			}
			/* pause, then restart work-request loop */
			if (!opt_benchmark) {
				applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
				return false;
			}
			//		sleep(opt_fail_pause);
		}
	}
	return true;
}

static void *workio_thread(void *userdata)
{

	struct thr_info *mythr = (struct thr_info*)userdata;
	CURL *curl;
	bool ok = true;

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		applog(LOG_ERR, "CURL initialization failed");
		return NULL;
	}

	while (ok && !abort_flag) {
		struct workio_cmd *wc;

		/* wait for workio_cmd sent to us, on our queue */
		wc = (struct workio_cmd *)tq_pop(mythr->q, NULL);
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
			if (opt_led_mode == LED_MODE_SHARES)
				gpu_led_on(device_map[wc->thr->id]);
			ok = workio_submit_work(wc, curl);
			if (opt_led_mode == LED_MODE_SHARES)
				gpu_led_off(device_map[wc->thr->id]);
			break;
		case WC_ABORT:
		default:		/* should never happen */
			ok = false;
			break;
		}

		if (!ok /*&& num_pools > 1 && opt_pool_failover*/) {
//			if (opt_debug_threads)
				applog(LOG_DEBUG, "%s died, failover", __func__);
		if (num_pools>1)
			ok = pool_switch_next(-1);
		else
			ok = pool_retry(0);
		
			tq_push(wc->thr->q, NULL); // get_work() will return false
		}

		workio_cmd_free(wc);
	}

	if (opt_debug_threads)
		applog(LOG_DEBUG, "%s() died", __func__);
	curl_easy_cleanup(curl);
	tq_freeze(mythr->q);
	return NULL;
}

bool get_work(struct thr_info *thr, struct work *work)
{
	struct workio_cmd *wc;
	struct work *work_heap;

	if (opt_benchmark) {
		memset(work->data, 0x55, 76);
		//work->data[17] = swab32((uint32_t)time(NULL));
		memset(work->data + 19, 0x00, 52);
		if (opt_algo == ALGO_DECRED) {
			memset(&work->data[35], 0x00, 52);
		} else if (opt_algo == ALGO_LBRY) {
			work->data[28] = 0x80000000;
		} else {
			work->data[20] = 0x80000000;
			work->data[31] = 0x00000280;
		}
		memset(work->target, 0x00, sizeof(work->target));
		return true;
	}

	/* fill out work request message */
	wc = (struct workio_cmd *)calloc(1, sizeof(*wc));
	if (!wc)
		return false;

	wc->cmd = WC_GET_WORK;
	wc->thr = thr;
	wc->pooln = cur_pooln;

	/* send work request to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc)) {
		workio_cmd_free(wc);
		return false;
	}

	/* wait for response, a unit of work */
	work_heap = (struct work *)tq_pop(thr->q, NULL);
	if (!work_heap)
		return false;

	/* copy returned work into storage provided by caller */
	memcpy(work, work_heap, sizeof(*work));
	aligned_free(work_heap);

	return true;
}

static bool submit_work(struct thr_info *thr, const struct work *work_in)
{
	struct workio_cmd *wc;
	/* fill out work request message */
	wc = (struct workio_cmd *)calloc(1, sizeof(*wc));
	if (!wc)
		return false;

	wc->u.work = (struct work *)aligned_calloc(sizeof(*work_in));
	if (!wc->u.work)
		goto err_out;

	wc->cmd = WC_SUBMIT_WORK;
	wc->thr = thr;
	memcpy(wc->u.work, work_in, sizeof(struct work));
	wc->pooln = work_in->pooln;

	/* send solution to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc))
		goto err_out;

	return true;

err_out:
	workio_cmd_free(wc);
	return false;
}


static bool submit_work_mtp(struct thr_info *thr, const struct work *work_in, const struct mtp *mtp_in)
{

	struct workio_cmd *wc; // = (struct workio_cmd*)malloc(sizeof(workio_cmd*));
						   /* fill out work request message */
	wc = (struct workio_cmd *)calloc(1, sizeof(*wc));
	if (!wc)
		return false;

	wc->u.work = (struct work *)aligned_calloc(sizeof(*work_in));

	wc->t.mtp = (struct mtp *)aligned_calloc(sizeof(*mtp_in));
//	wc->t.mtp = (struct mtp *)malloc(sizeof(*mtp_in));
	if (!wc->u.work)
		goto err_out;

	if (!wc->t.mtp)
		goto err_out;

	wc->cmd = WC_SUBMIT_WORK;
	wc->thr = thr;
	memcpy(wc->u.work, work_in, sizeof(struct work));
	memcpy(wc->t.mtp, mtp_in, sizeof(struct mtp));
	//	for (int i=0;i<(2048*8*8*8);i++)
	//		wc->t.mtp->mtpproof[i] = mtp_in->mtpproof[i];

	//	memcpy(wc->mtp, mtp_in, sizeof(struct mtp));

	wc->pooln = work_in->pooln;

	/* send solution to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc))
		goto err_out;

	return true;

err_out:
	workio_cmd_free(wc);
	return false;
}




static bool stratum_gen_work_m7(struct stratum_ctx *sctx, struct work *work)
{

	if (!sctx->job.job_id) {
	    applog(LOG_WARNING, "stratum_gen_work: job not yet retrieved");
		return false;
	}


	pthread_mutex_lock(&stratum_work_lock);

//	strcpy(work->job_id, sctx->job.job_id);

	snprintf(work->job_id, sizeof(work->job_id), "%07x %s",
		be32dec(sctx->job.ntime) & 0xfffffff, sctx->job.job_id);

	work->xnonce2_len = sctx->xnonce2_size;
	memcpy(work->xnonce2, sctx->job.xnonce2, sctx->xnonce2_size);

	/* Increment extranonce2 */
	for (int i = 0; i < (int)sctx->xnonce2_size && !++sctx->job.xnonce2[i]; i++);

	/* Assemble block header */
	memset(work->data, 0, 122);
	memcpy(work->data, sctx->job.m7prevblock, 32);
	memcpy(work->data + 8, sctx->job.m7accroot, 32);
	memcpy(work->data + 16, sctx->job.m7merkleroot, 32);
	((uint64_t*)work->data)[12] = be64dec(sctx->job.m7ntime);
	((uint64_t*)work->data)[13] = be64dec(sctx->job.m7height);
	unsigned char *xnonce_ptr = (unsigned char *)(work->data + 28);
	for (int i = 0; i < (int)sctx->xnonce1_size; i++) {
		*(xnonce_ptr + i) = sctx->xnonce1[i];
	}
	for (int i = 0; i < (int)work->xnonce2_len; i++) {
		*(xnonce_ptr + sctx->xnonce1_size + i) = work->xnonce2[i];
	}
	((uint16_t*)work->data)[60] = be16dec(sctx->job.m7version);

	pthread_mutex_unlock(&stratum_work_lock);

	diff_to_target(work->target, sctx->job.diff / (65536.0* opt_difficulty));

//let that people who want to fix this...
	if (opt_debug) {
		char *data_str = bin2hex((unsigned char *)work->data, 122);
		applog(LOG_DEBUG, "DEBUG: stratum_gen_work data %s", data_str);
		char *target_str = bin2hex((unsigned char *)work->target, 32);
		applog(LOG_DEBUG, "DEBUG: stratum_gen_work target %s", target_str);
	}

	if (stratum_diff != sctx->job.diff) {
		char sdiff[32] = { 0 };
		// store for api stats
		stratum_diff = sctx->job.diff;
		if (opt_showdiff && work->targetdiff != stratum_diff)
			snprintf(sdiff, 32, " (%.5f)", work->targetdiff);
		applog(LOG_WARNING, "Stratum difficulty set to %g%s", stratum_diff, sdiff);
	}

return true;
}

static bool increment_X2(struct work *work)
{
	int i;
	pthread_mutex_lock(&stratum_work_lock);
	//	memcpy(work->xnonce2, sctx->job.xnonce2, sctx->xnonce2_size);
	for (i = 0; i < (int)work->xnonce2_len && !++work->xnonce2[i]; i++);

	pthread_mutex_unlock(&stratum_work_lock);
	return true;
}

static bool stratum_gen_work(struct stratum_ctx *sctx, struct work *work)
{
	uchar merkle_root[64] = { 0 };
	int i;

	if (!sctx->job.job_id) {
		applog(LOG_WARNING, "stratum_gen_work: job not yet retrieved");
		return false;
	}

	pthread_mutex_lock(&stratum_work_lock);


//		printf("stratumgen \n");

	// store the job ntime as high part of jobid
//	free(work->job_id);
//	work->job_id = strdup(sctx->job.job_id);
//	for (int i=0;i<4;i++)
//	work->job_iduc[i] = sctx->job.ucjob_id[i];
/* Increment extranonce2 */
	if (sctx->job.IncXtra) {

	for (i = 0; i < (int)sctx->xnonce2_size && !++sctx->job.xnonce2[i]; i++);
	sctx->job.IncXtra = false;
	}


	snprintf(work->job_id, sizeof(work->job_id), "%07x %s",
		be32dec(sctx->job.ntime) & 0xfffffff, sctx->job.job_id);
	work->xnonce2_len = sctx->xnonce2_size;
	memcpy(work->xnonce2, sctx->job.xnonce2, sctx->xnonce2_size);

//	printf("thefull job id %s\n",work->job_id);
//	printf("reduced job id %s\n", work->job_id+8); 
/*
	unsigned char* Transfer = (unsigned char*)malloc(sctx->job.coinbase_size);
	if (opt_algo == ALGO_MTP) {
		memcpy(Transfer, sctx->job.coinbase, sctx->job.coinbase_size);
		memcpy(Transfer + 60, work->xnonce2, 8);
	}
*/



	// also store the block number
	work->height = sctx->job.height;
	// and the pool of the current stratum
	work->pooln = sctx->pooln;

	/* Generate merkle root */
	switch (opt_algo) {
		case ALGO_DECRED:
		case ALGO_SIA:
			// getwork over stratum, no merkle to generate
			break;
		case ALGO_HEAVY:
		case ALGO_MJOLLNIR:
			heavycoin_hash(merkle_root, sctx->job.coinbase, (int)sctx->job.coinbase_size);
			break;
		case ALGO_FUGUE256:
		case ALGO_GROESTL:
		case ALGO_KECCAK:
		case ALGO_BLAKECOIN:
		case ALGO_WHIRLCOIN:
			SHA256((uchar*)sctx->job.coinbase, sctx->job.coinbase_size, (uchar*)merkle_root);
			break;
		case ALGO_WHIRLPOOL:
		default:
			sha256d(merkle_root, sctx->job.coinbase, (int)sctx->job.coinbase_size);
	}

	for (i = 0; i < sctx->job.merkle_count; i++) {
		memcpy(merkle_root + 32, sctx->job.merkle[i], 32);
		if (opt_algo == ALGO_HEAVY || opt_algo == ALGO_MJOLLNIR)
			heavycoin_hash(merkle_root, merkle_root, 64);
		else
			sha256d(merkle_root, merkle_root, 64);
	}
	
	/* Increment extranonce2 */
//printf("incrementing nonce\n");
//	for (i = 0; i < (int)sctx->xnonce2_size && !++sctx->job.xnonce2[i]; i++);

	/* Assemble block header */
	memset(work->data, 0, sizeof(work->data));
	work->data[0] = le32dec(sctx->job.version);
	for (i = 0; i < 8; i++)
		work->data[1 + i] = le32dec((uint32_t *)sctx->job.prevhash + i);

	if (opt_algo == ALGO_DECRED) {
		uint16_t vote;
		for (i = 0; i < 8; i++) // reversed prevhash
			work->data[1 + i] = swab32(work->data[1 + i]);
		// decred header (coinb1) [merkle...nonce]
		memcpy(&work->data[9], sctx->job.coinbase, 108);
		// last vote bit should never be changed
		memcpy(&vote, &work->data[25], 2);
		vote = (opt_vote << 1) | (vote & 1);
		memcpy(&work->data[25], &vote, 2);
		// extradata
		if (sctx->xnonce1_size > sizeof(work->data)-(36*4)) {
			// should never happen...
			applog(LOG_ERR, "extranonce size overflow!");
			sctx->xnonce1_size = sizeof(work->data)-(36*4);
		}
		memcpy(&work->data[36], sctx->xnonce1, sctx->xnonce1_size);
		work->data[37] = (rand()*4) << 8; // random work data
		sctx->job.height = work->data[32];
		//applog_hex(work->data, 180);
	} else if (opt_algo == ALGO_LBRY) {
		for (i = 0; i < 8; i++)
			work->data[9 + i] = be32dec((uint32_t *)merkle_root + i);
		for (i = 0; i < 8; i++)
			work->data[17 + i] = ((uint32_t*)sctx->job.claim)[i];
		work->data[25] = le32dec(sctx->job.ntime);
		work->data[26] = le32dec(sctx->job.nbits);
		work->data[28] = 0x80000000;
	} else if (opt_algo == ALGO_SIA) {
		uint32_t extra = 0;
		memcpy(&extra, &sctx->job.coinbase[32], 2);
		for (i = 0; i < 8; i++) // reversed hash
			work->data[i] = ((uint32_t*)sctx->job.prevhash)[7-i];
		work->data[8] = 0; // nonce
		work->data[9] = swab32(extra) | ((rand() << 8) & 0xffff);
		work->data[10] = be32dec(sctx->job.ntime);
		work->data[11] = be32dec(sctx->job.nbits);
		memcpy(&work->data[12], sctx->job.coinbase, 32); // merkle_root
		work->data[20] = 0x80000000;
		if (opt_debug) applog_hex(work->data, 80);
	} else if (opt_algo == ALGO_MTP || opt_algo == ALGO_MTPTCR)
	{
		for (i = 0; i < 8; i++)
			work->data[9 + i] = le32dec((uint32_t *)merkle_root + i);
	
		work->data[17] = le32dec(sctx->job.ntime);
		work->data[18] = le32dec(sctx->job.nbits);
		work->data[20] = 0x00100000;

//		for (int k = 0; k < 20; k++) {
//			printf(" %08x", work->data[k]);
//		}
//		printf("\n");
	} else {
		for (i = 0; i < 8; i++)
			work->data[9 + i] = be32dec((uint32_t *)merkle_root + i);
		work->data[17] = le32dec(sctx->job.ntime);
		work->data[18] = le32dec(sctx->job.nbits);
		work->data[20] = 0x80000000;
		work->data[31] = (opt_algo == ALGO_MJOLLNIR) ? 0x000002A0 : 0x00000280;
	}

	if (opt_showdiff || opt_max_diff > 0.)
		calc_network_diff(work);

	switch (opt_algo) {
	case ALGO_MJOLLNIR:
	case ALGO_HEAVY:
	case ALGO_ZR5:
		for (i = 0; i < 20; i++)
			work->data[i] = swab32(work->data[i]);
		break;
	}

	// HeavyCoin (vote / reward)
	if (opt_algo == ALGO_HEAVY) {
		work->maxvote = 2048;
		uint16_t *ext = (uint16_t*)(&work->data[20]);
		ext[0] = opt_vote;
		ext[1] = be16dec(sctx->job.nreward);
		// applog(LOG_DEBUG, "DEBUG: vote=%hx reward=%hx", ext[0], ext[1]);
	}

	pthread_mutex_unlock(&stratum_work_lock);

	if (opt_debug && opt_algo != ALGO_DECRED && opt_algo != ALGO_SIA) {
		uint32_t utm = work->data[17];
		if (opt_algo != ALGO_ZR5) utm = swab32(utm);
		char *tm = atime2str(utm - sctx->srvtime_diff);
		char *xnonce2str = bin2hex(work->xnonce2, sctx->xnonce2_size);
		applog(LOG_DEBUG, "DEBUG: job_id=%s xnonce2=%s time=%s",
		       work->job_id, xnonce2str, tm);
		free(tm);
		free(xnonce2str);
	}

	if (opt_difficulty == 0.)
		opt_difficulty = 1.;

	switch (opt_algo) {
		case ALGO_MTP:
		case ALGO_MTPTCR:
				work_set_target_mtp(work, sctx->next_target, sctx->job.diff);
			break;
		case ALGO_JACKPOT:
		case ALGO_NEOSCRYPT:
		case ALGO_SCRYPT:
		case ALGO_SCRYPT_JANE:
			work_set_target(work, sctx->job.diff / (65536.0 * opt_difficulty));
			break;
		case ALGO_DMD_GR:
		case ALGO_FRESH:
		case ALGO_FUGUE256:
		case ALGO_GROESTL:
		case ALGO_LBRY:
		case ALGO_LYRA2Z:
		case ALGO_LYRA2v2:
			work_set_target(work, sctx->job.diff / (256.0 * opt_difficulty));
			break;
		case ALGO_KECCAK:
		case ALGO_LYRA2:
			work_set_target(work, sctx->job.diff / (128.0 * opt_difficulty));
			break;
		default:
			work_set_target(work, sctx->job.diff / opt_difficulty);
	}

	if (stratum_diff != sctx->job.diff) {
		char sdiff[32] = { 0 };
		// store for api stats
		stratum_diff = sctx->job.diff;
		if (opt_showdiff && work->targetdiff != stratum_diff)
			snprintf(sdiff, 32, " (%.5f)", work->targetdiff);
		applog(LOG_WARNING, "Stratum difficulty set to %g%s", stratum_diff, sdiff);
	}

	return true;
}

void restart_threads(void)
{
	if (opt_debug && !opt_quiet)
		applog(LOG_DEBUG,"%s", __FUNCTION__);

	for (int i = 0; i < opt_n_threads && work_restart; i++)
		work_restart[i].restart = 1;
}

static bool wanna_mine(int thr_id)
{
	bool state = true;
	bool allow_pool_rotate = (thr_id == 0 && num_pools > 1 && !pool_is_switching);

	if (opt_max_temp > 0.0) {
#ifdef USE_WRAPNVML
		struct cgpu_info * cgpu = &thr_info[thr_id].gpu;
		float temp = gpu_temp(cgpu);
		if (temp > opt_max_temp) {
			if (!conditional_state[thr_id] && !opt_quiet)
				gpulog(LOG_INFO, thr_id, "temperature too high (%.0f°c), waiting...", temp);
			state = false;
		} else if (opt_max_temp > 0. && opt_resume_temp > 0. && conditional_state[thr_id] && temp > opt_resume_temp) {
			if (!thr_id && opt_debug)
				applog(LOG_DEBUG, "temperature did not reach resume value %.1f...", opt_resume_temp);
			state = false;
		}
#endif
	}
	// Network Difficulty
	if (opt_max_diff > 0.0 && net_diff > opt_max_diff) {
		int next = pool_get_first_valid(cur_pooln+1);
		if (num_pools > 1 && pools[next].max_diff != pools[cur_pooln].max_diff && opt_resume_diff <= 0.)
			conditional_pool_rotate = allow_pool_rotate;
		if (!thr_id && !conditional_state[thr_id] && !opt_quiet)
			applog(LOG_INFO, "network diff too high, waiting...");
		state = false;
	} else if (opt_max_diff > 0. && opt_resume_diff > 0. && conditional_state[thr_id] && net_diff > opt_resume_diff) {
		if (!thr_id && opt_debug)
			applog(LOG_DEBUG, "network diff did not reach resume value %.3f...", opt_resume_diff);
		state = false;
	}
	// Network hashrate
	if (opt_max_rate > 0.0 && net_hashrate > opt_max_rate) {
		int next = pool_get_first_valid(cur_pooln+1);
		if (pools[next].max_rate != pools[cur_pooln].max_rate && opt_resume_rate <= 0.)
			conditional_pool_rotate = allow_pool_rotate;
		if (!thr_id && !conditional_state[thr_id] && !opt_quiet) {
			char rate[32];
			format_hashrate(opt_max_rate, rate);
			applog(LOG_INFO, "network hashrate too high, waiting %s...", rate);
		}
		state = false;
	} else if (opt_max_rate > 0. && opt_resume_rate > 0. && conditional_state[thr_id] && net_hashrate > opt_resume_rate) {
		if (!thr_id && opt_debug)
			applog(LOG_DEBUG, "network rate did not reach resume value %.3f...", opt_resume_rate);
		state = false;
	}
	conditional_state[thr_id] = (uint8_t) !state; // only one wait message in logs
	return state;
}

static void *miner_thread(void *userdata)
{

	struct thr_info *mythr = (struct thr_info *)userdata;
//	struct mtp * mtp = (struct mtp*)malloc(sizeof(struct mtp));
	int switchn = pool_switch_count;
	int thr_id = mythr->id;
	int dev_id = device_map[thr_id % MAX_GPUS];
	stratum_ctx* ctx = &stratum;

	struct work work;
	struct mtp mtp;
	uint64_t loopcnt = 0;
	uint32_t max_nonce;
	uint32_t end_nonce = UINT32_MAX / opt_n_threads * (thr_id + 1) ;
	time_t tm_rate_log = 0;
	bool work_done = false;
	bool extrajob = false;
	char s[16];
	int rc = 0;

//	memset(&work, 0, sizeof(work)); // prevent work from being used uninitialized

	if (opt_priority > 0) {
		int prio = 2; // default to normal
#ifndef WIN32
		prio = 0;
		// note: different behavior on linux (-19 to 19)
		switch (opt_priority) {
			case 0:
				prio = 15;
				break;
			case 1:
				prio = 5;
				break;
			case 2:
				prio = 0; // normal process
				break;
			case 3:
				prio = -1; // above
				break;
			case 4:
				prio = -10;
				break;
			case 5:
				prio = -15;
		}
		if (opt_debug)
			applog(LOG_DEBUG, "Thread %d priority %d (nice %d)",
				thr_id,	opt_priority, prio);
#endif
		setpriority(PRIO_PROCESS, 0, prio);
		drop_policy();
	}

	/* Cpu thread affinity */

	if (num_cpus > 1) {

		if (opt_affinity == -1L && opt_n_threads > 1) {
//			if (opt_debug)
				applog(LOG_DEBUG, "Binding thread %d to cpu %d (mask %x)", thr_id,
						thr_id % num_cpus, (1UL << (thr_id % num_cpus)));
			affine_to_cpu_mask(thr_id, 1 << (thr_id % num_cpus));
		} else if (opt_affinity != -1L) {
//			if (opt_debug)
				applog(LOG_DEBUG, "Test Binding thread %d to cpu mask %lx", thr_id,
						(long) opt_affinity);
			affine_to_cpu_mask(thr_id, (unsigned long) opt_affinity);
		}

	}

	gpu_led_off(dev_id);

	while (!abort_flag) {
//		struct mtp * mtp = (struct mtp*)malloc(sizeof(struct mtp));
		struct timeval tv_start, tv_end, diff;
		unsigned long hashes_done;
		uint32_t start_nonce;
		uint32_t scan_time = have_longpoll ? LP_SCANTIME : opt_scantime;
		uint64_t max64, minmax = 0x100000;
		int nodata_check_oft = 0;
		bool regen = false;
		int wcmplen;
		int wcmpoft;
		switch(opt_algo)
		{
			case ALGO_M7:
				wcmplen = 116;
				wcmpoft = 0;
			break;
			case ALGO_DECRED:
				wcmplen = 140;
				wcmpoft = 0;
			break;
			case ALGO_LBRY:
				wcmplen = 108;
				wcmpoft = 0;
			break;
			case ALGO_SIA:
				wcmpoft = (32 + 16) / 4;
				wcmplen = 32;
			break;
			case ALGO_MTP:
			case ALGO_MTPTCR:
				wcmpoft = 0;
				wcmplen = 84;
				break;
			default:
				wcmplen = 76;
				wcmpoft = 0;
			break;
		}


		uint32_t *nonceptr = (uint32_t*) (((char*)work.data) + wcmplen);

		if (have_stratum) {
			uint32_t sleeptime = 0;

			if (opt_algo == ALGO_DECRED)
				work_done = true; // force "regen" hash
			while (!work_done && time(NULL) >= (g_work_time + opt_scantime)) {
				usleep(100*1000);
				if (sleeptime > 4) {
					extrajob = true;
					break;
				}
				sleeptime++;
			}
			if (sleeptime && opt_debug && !opt_quiet)
				applog(LOG_DEBUG, "sleeptime: %u ms", sleeptime*100);

			if (opt_algo==ALGO_MTP  || opt_algo == ALGO_MTPTCR)
				nonceptr = (uint32_t*) (((char*)work.data) + 76);
			else 
				nonceptr = (uint32_t*)(((char*)work.data) + wcmplen);
			pthread_mutex_lock(&g_work_lock);
			extrajob |= work_done;

			regen = (nonceptr[0] >= end_nonce);



			if (opt_algo == ALGO_SIA) {
				regen = ((nonceptr[1] & 0xFF00) >= 0xF000);
			}
			regen = regen || extrajob;
			if (!regen)
			if (strncmp(stratum.job.job_id, g_work.job_id + 8, sizeof(g_work.job_id) - 8) != 0)
			{
				regen = true;
			}
			if (regen || opt_algo == ALGO_MTP || opt_algo == ALGO_MTPTCR) {
				

				work_done = false;
				extrajob = false;
			if (opt_algo == ALGO_M7) {
				if (stratum_gen_work_m7(&stratum, &g_work)) g_work_time = time(NULL);}
			else {
				if (stratum_gen_work(&stratum, &g_work)) g_work_time = time(NULL);}
			}
		} else {
			uint32_t secs = 0;
			pthread_mutex_lock(&g_work_lock);
			secs = (uint32_t) (time(NULL) - g_work_time);
			if (secs >= scan_time || nonceptr[0] >= (end_nonce - 0x100)) {
				if (opt_debug && g_work_time && !opt_quiet)
					applog(LOG_DEBUG, "work time %u/%us nonce %x/%x", secs, scan_time, nonceptr[0], end_nonce);
				/* obtain new work from internal workio thread */
			
				if (unlikely(!get_work(mythr, &g_work))) {
					pthread_mutex_unlock(&g_work_lock);
					if (switchn != pool_switch_count) {
						switchn = pool_switch_count;
						continue;
					} else {
						applog(LOG_ERR, "work retrieval failed, exiting mining thread %d", mythr->id);
						goto out;
					}
				}
			
				g_work_time = time(NULL);
			}
		}

		if (!opt_benchmark && (g_work.height != work.height || memcmp(work.target, g_work.target, sizeof(work.target))))
		{
			if (opt_debug) {
				uint64_t target64 = g_work.target[7] * 0x100000000ULL + g_work.target[6];
				applog(LOG_DEBUG, "job %s target change: %llx (%.1f)", g_work.job_id, target64, g_work.targetdiff);
			}
			memcpy(work.target, g_work.target, sizeof(work.target));
			work.targetdiff = g_work.targetdiff;
			work.height = g_work.height;
			//nonceptr[0] = (UINT32_MAX / opt_n_threads) * thr_id; // 0 if single thr
		}

		if (opt_algo == ALGO_ZR5) {
			// ignore pok/version header
			wcmpoft = 1;
			wcmplen -= 4;
		}

		if (memcmp(&work.data[wcmpoft], &g_work.data[wcmpoft], wcmplen - 8)) {
			#if 0
			if (opt_debug) {
				for (int n=0; n <= (wcmplen-8); n+=8) {
					if (memcmp(work.data + n, g_work.data + n, 8)) {
						applog(LOG_DEBUG, "job %s work updated at offset %d:", g_work.job_id, n);
						applog_hash((uchar*) &work.data[n]);
						applog_compare_hash((uchar*) &g_work.data[n], (uchar*) &work.data[n]);
					}
				}
			}
			#endif


			memcpy(&work, &g_work, sizeof(struct work));

			nonceptr[0] = (UINT32_MAX / opt_n_threads) * thr_id; // 0 if single thr
		
		} else {
			nonceptr[0]++; //??
//			nonceptr[0] = (UINT32_MAX / opt_n_threads) * thr_id; // 0 if single thr
		}
		if (opt_algo == ALGO_DECRED) {
			// suprnova job_id check without data/target/height change...
			if (check_stratum_jobs && strcmp(work.job_id, g_work.job_id)) {
				pthread_mutex_unlock(&g_work_lock);
				continue;
			}

			// use the full range per loop
			nonceptr[0] = 0;
			end_nonce = UINT32_MAX;
			// and make an unique work (extradata)
			nonceptr[1] += 1;
			nonceptr[2] |= thr_id;

		} else if (opt_algo == ALGO_SIA) {
			// suprnova job_id check without data/target/height change...
			if (have_stratum && strcmp(work.job_id, g_work.job_id)) {
				pthread_mutex_unlock(&g_work_lock);
				work_done = true;
				continue;
			}
			nonceptr[1] += opt_n_threads;
			nonceptr[1] |= thr_id;
			// range max
			nonceptr[0] = 0;
			end_nonce = UINT32_MAX;
		} else if (opt_benchmark) {
			// randomize work
			nonceptr[-1] += 1;
		}

		pthread_mutex_unlock(&g_work_lock);

		// --benchmark [-a all]
		if (opt_benchmark && bench_algo >= 0) {
			//gpulog(LOG_DEBUG, thr_id, "loop %d", loopcnt);
			if (loopcnt >= 3) {
				if (!bench_algo_switch_next(thr_id) && thr_id == 0)
				{
					bench_display_results();
					proper_exit(0);
					break;
				}
				loopcnt = 0;
			}
		}
		loopcnt++;

		// prevent gpu scans before a job is received
		if (opt_algo == ALGO_SIA) nodata_check_oft = 7; // no stratum version
		else if (opt_algo == ALGO_DECRED) nodata_check_oft = 4; // testnet ver is 0
		else nodata_check_oft = 0;
		if (have_stratum && work.data[nodata_check_oft] == 0 && !opt_benchmark) {
			sleep(1);
			if (!thr_id) pools[cur_pooln].wait_time += 1;
			gpulog(LOG_DEBUG, thr_id, "no data");
			continue;
		}

		/* conditional mining */
		if (!wanna_mine(thr_id)) {

			// free gpu resources

			algo_free_all(thr_id);
			// clear any free error (algo switch)
			cuda_clear_lasterror();

			// conditional pool switch
			if (num_pools > 1 && conditional_pool_rotate) {
				if (!pool_is_switching)
					pool_switch_next(thr_id);
				else if (time(NULL) - firstwork_time > 35) {
					if (!opt_quiet)
						applog(LOG_WARNING, "Pool switching timed out...");
					if (!thr_id) pools[cur_pooln].wait_time += 1;
					pool_is_switching = false;
				}
				sleep(1);
				continue;
			}

			pool_on_hold = true;
			global_hashrate = 0;
			sleep(5);
			if (!thr_id) pools[cur_pooln].wait_time += 5;
			continue;
		}
		pool_on_hold = false;

		work_restart[thr_id].restart = 0;

		/* adjust max_nonce to meet target scan time */
		if (have_stratum)
			max64 = LP_SCANTIME;
		else
			max64 = max(1, (int64_t) scan_time + g_work_time - time(NULL));

		/* time limit */
		if (opt_time_limit > 0 && firstwork_time) {
			int passed = (int)(time(NULL) - firstwork_time);
			int remain = (int)(opt_time_limit - passed);
			if (remain < 0)  {
				if (thr_id != 0) {
					sleep(1); continue;
				}
				if (num_pools > 1 && pools[cur_pooln].time_limit > 0) {
					if (!pool_is_switching) {
						if (!opt_quiet)
							applog(LOG_INFO, "Pool mining timeout of %ds reached, rotate...", opt_time_limit);
						pool_switch_next(thr_id);
					} else if (passed > 35) {
						// ensure we dont stay locked if pool_is_switching is not reset...
						applog(LOG_WARNING, "Pool switch to %d timed out...", cur_pooln);
						if (!thr_id) pools[cur_pooln].wait_time += 1;
						pool_is_switching = false;
					}
					sleep(1);
					continue;
				}
				app_exit_code = EXIT_CODE_TIME_LIMIT;
				abort_flag = true;
				if (opt_benchmark) {
					char rate[32];
					format_hashrate((double)global_hashrate, rate);
					applog(LOG_NOTICE, "Benchmark: %s", rate);
					usleep(200*1000);
					fprintf(stderr, "%llu\n", (long long unsigned int) global_hashrate);
				} else {
					applog(LOG_NOTICE, "Mining timeout of %ds reached, exiting...", opt_time_limit);
				}
				workio_abort();
				break;
			}
			if (remain < max64) max64 = remain;
		}

		/* shares limit */
/*
		if (opt_shares_limit > 50 && firstwork_time) {

printf("coming here opt_shares_limit firstwork_time=%d\n",firstwork_time);
			int64_t shares = (pools[cur_pooln].accepted_count + pools[cur_pooln].rejected_count);
			if (shares >= opt_shares_limit) {
				int passed = (int)(time(NULL) - firstwork_time);
				if (thr_id != 0) {
					sleep(1); continue;
				}
				if (num_pools > 1 && pools[cur_pooln].shares_limit > 0) {
					if (!pool_is_switching) {
						if (!opt_quiet)
							applog(LOG_INFO, "Pool shares limit of %d reached, rotate...", opt_shares_limit);
						pool_switch_next(thr_id);
						
						stratum_disconnect(&stratum);
						stratum_need_reset = true;
//						tq_freeze(mythr->q);
//						return NULL;
					} else if (passed > 35) {
						// ensure we dont stay locked if pool_is_switching is not reset...
						applog(LOG_WARNING, "Pool switch to %d timed out...", cur_pooln);
						if (!thr_id) pools[cur_pooln].wait_time += 1;
						pool_is_switching = false;
					}
					sleep(1);
					continue;
				}
				abort_flag = true;
				app_exit_code = EXIT_CODE_OK;
				applog(LOG_NOTICE, "Mining limit of %d shares reached, exiting...", opt_shares_limit);
				workio_abort();
				break;
			}
		}
*/
		max64 *= (uint32_t)thr_hashrates[thr_id];

		/* on start, max64 should not be 0,
		 *    before hashrate is computed */
		if (max64 < minmax) {
			switch (opt_algo) {
			case ALGO_M7:
				max64 = 0x3ffffULL;
				break;
			case ALGO_BLAKECOIN:
			case ALGO_BLAKE2S:
			case ALGO_VANILLA:
				minmax = 0x80000000U;
				break;
			case ALGO_BLAKE:
			case ALGO_BMW:
			case ALGO_DECRED:
			//case ALGO_WHIRLPOOLX:
				minmax = 0x40000000U;
				break;
			case ALGO_KECCAK:
			case ALGO_LBRY:
			case ALGO_LUFFA:
			case ALGO_SIA:
			case ALGO_SKEIN:
			case ALGO_SKEIN2:
				minmax = 0x1000000;
				break;
			case ALGO_C11:
			case ALGO_DEEP:
			case ALGO_HEAVY:
			case ALGO_MTP:
			case ALGO_MTPTCR:
			case ALGO_LYRA2v2:
			case ALGO_LYRA2Z:
			case ALGO_S3:
			case ALGO_X11EVO:
			case ALGO_X11:
			case ALGO_X13:
			case ALGO_WHIRLCOIN:
			case ALGO_WHIRLPOOL:
				minmax = 0x400000;
				break;
			case ALGO_JACKPOT:
			case ALGO_X14:
			case ALGO_X15:
				minmax = 0x300000;
				break;
			case ALGO_LYRA2:

			case ALGO_NEOSCRYPT:
			case ALGO_SIB:
			case ALGO_SCRYPT:
			case ALGO_VELTOR:
				minmax = 0x80000;
				break;
			case ALGO_SCRYPT_JANE:
				minmax = 0x1000;
				break;
			}
			max64 = max(minmax-1, max64);
		}

		// we can't scan more than uint32 capacity
		max64 = min(UINT32_MAX, max64);

		start_nonce = nonceptr[0];

		/* never let small ranges at end */
		if (end_nonce >= UINT32_MAX - 256)
			end_nonce = UINT32_MAX;

		if ((max64 + start_nonce) >= end_nonce)
			max_nonce = end_nonce;
		else
			max_nonce = (uint32_t) (max64 + start_nonce);

		// todo: keep it rounded to a multiple of 256 ?

		if (unlikely(start_nonce > max_nonce)) {
			// should not happen but seen in skein2 benchmark with 2 gpus
			max_nonce = end_nonce = UINT32_MAX;
		}

		work.scanned_from = start_nonce;

//		gpulog(LOG_DEBUG, thr_id, "start=%08x end=%08x range=%08x",
//			start_nonce, max_nonce, (max_nonce-start_nonce));

		if (opt_led_mode == LED_MODE_MINING)
			gpu_led_on(dev_id);

		hashes_done = 0;
		gettimeofday(&tv_start, NULL);

		// check (and reset) previous errors
		cudaError_t err = cudaGetLastError();
		if (err != cudaSuccess && !opt_quiet)
			gpulog(LOG_WARNING, thr_id, "%s", cudaGetErrorString(err));

//		memcpy(work.job_id,g_work.job_id,128);

		/* scan nonces for a proof-of-work hash */
		switch (opt_algo) {

		case ALGO_BLAKECOIN:
			rc = scanhash_blake256(thr_id, &work, max_nonce, &hashes_done, 8);
			break;
		case ALGO_BLAKE:
			rc = scanhash_blake256(thr_id, &work, max_nonce, &hashes_done, 14);
			break;
		case ALGO_BLAKE2S:
			rc = scanhash_blake2s(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_BMW:
			rc = scanhash_bmw(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_C11:
			rc = scanhash_c11(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_DECRED:
			//applog(LOG_BLUE, "version %x, nbits %x, ntime %x extra %x",
			//	work.data[0], work.data[29], work.data[34], work.data[38]);
			rc = scanhash_decred(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_DEEP:
			rc = scanhash_deep(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_FRESH:
			rc = scanhash_fresh(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_FUGUE256:
			rc = scanhash_fugue256(thr_id, &work, max_nonce, &hashes_done);
			break;

		case ALGO_GROESTL:
		case ALGO_DMD_GR:
			rc = scanhash_groestlcoin(thr_id, &work, max_nonce, &hashes_done);
			break;

		case ALGO_M7:
			rc = scanhash_m7(thr_id, work.data, work.target, max_nonce, &hashes_done);
			break;

		case ALGO_MYR_GR:
			rc = scanhash_myriad(thr_id, &work, max_nonce, &hashes_done);
			break;

		case ALGO_HEAVY:
			rc = scanhash_heavy(thr_id, &work, max_nonce, &hashes_done, work.maxvote, HEAVYCOIN_BLKHDR_SZ);
			break;
		case ALGO_MJOLLNIR:
			rc = scanhash_heavy(thr_id, &work, max_nonce, &hashes_done, 0, MNR_BLKHDR_SZ);
			break;
		case ALGO_KECCAK:
			rc = scanhash_keccak256(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_JACKPOT:
			rc = scanhash_jackpot(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_LBRY:
			rc = scanhash_lbry(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_LUFFA:
			rc = scanhash_luffa(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_QUARK:
			rc = scanhash_quark(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_QUBIT:
			rc = scanhash_qubit(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_MTP:
		if (!have_stratum)
			rc = scanhash_mtp_solo(opt_n_threads,thr_id, &work, max_nonce, &hashes_done, &mtp,&stratum);
		else 
			rc = scanhash_mtp(opt_n_threads, thr_id, &work, max_nonce, &hashes_done, &mtp, &stratum);
			break;

		case ALGO_MTPTCR:
			if (!have_stratum)
				rc = scanhash_mtptcr_solo(opt_n_threads, thr_id, &work, max_nonce, &hashes_done, &mtp, &stratum);
			else 
				rc = scanhash_mtptcr(opt_n_threads, thr_id, &work, max_nonce, &hashes_done, &mtp, &stratum);
			break;

		case ALGO_LYRA2:
			rc = scanhash_lyra2(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_LYRA2Z:
			rc = scanhash_lyra2Z(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_LYRA2v2:
			rc = scanhash_lyra2v2(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_NEOSCRYPT:
			rc = scanhash_neoscrypt(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_NIST5:
			rc = scanhash_nist5(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_PENTABLAKE:
			rc = scanhash_pentablake(thr_id, &work, max_nonce, &hashes_done);
			break;
/*
		case ALGO_SCRYPT:
			rc = scanhash_scrypt(thr_id, &work, max_nonce, &hashes_done,
				NULL, &tv_start, &tv_end);
			break;
		case ALGO_SCRYPT_JANE:
			rc = scanhash_scrypt_jane(thr_id, &work, max_nonce, &hashes_done,
				NULL, &tv_start, &tv_end);
			break;
*/
		case ALGO_SKEIN:
			rc = scanhash_skeincoin(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_SKEIN2:
			rc = scanhash_skein2(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_SIA:
			rc = scanhash_sia(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_SIB:
			rc = scanhash_sib(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_S3:
			rc = scanhash_s3(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_VANILLA:
			rc = scanhash_vanilla(thr_id, &work, max_nonce, &hashes_done, 8);
			break;
		case ALGO_VELTOR:
			rc = scanhash_veltor(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_WHIRLCOIN:
		case ALGO_WHIRLPOOL:
			rc = scanhash_whirl(thr_id, &work, max_nonce, &hashes_done);
			break;
		//case ALGO_WHIRLPOOLX:
		//	rc = scanhash_whirlx(thr_id, &work, max_nonce, &hashes_done);
		//	break;
		case ALGO_X11EVO:
			rc = scanhash_x11evo(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_X11:
			rc = scanhash_x11(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_X13:
			rc = scanhash_x13(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_X14:
			rc = scanhash_x14(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_X15:
			rc = scanhash_x15(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_X17:
			rc = scanhash_x17(thr_id, &work, max_nonce, &hashes_done);
			break;
		case ALGO_ZR5:
			rc = scanhash_zr5(thr_id, &work, max_nonce, &hashes_done);
			break;

		default:
			/* should never happen */
			goto out;
		}

		if (opt_led_mode == LED_MODE_MINING)
			gpu_led_off(dev_id);

		if (abort_flag)
			break; // time to leave the mining loop...

		if (work_restart[thr_id].restart)
			continue;

		/* record scanhash elapsed time */
		gettimeofday(&tv_end, NULL);

		// todo: update all algos to use work->nonces and pdata[19] as counter
		switch (opt_algo) {
			case ALGO_BLAKE2S:
			case ALGO_DECRED:
			case ALGO_LBRY:
			case ALGO_SIA:
			case ALGO_VELTOR:
				// migrated algos
				break;
			case ALGO_ZR5:
				// algos with only work.nonces[1] set
				work.nonces[0] = nonceptr[0];
				break;
			default:
				// algos with 2 results in pdata and work.nonces unset
				work.nonces[0] = nonceptr[0];
				work.nonces[1] = nonceptr[2];
		}

		if (rc > 0 && opt_debug)
			applog(LOG_NOTICE, CL_CYN "found => %08x" CL_GRN " %08x", work.nonces[0], swab32(work.nonces[0]));
		if (rc > 1 && opt_debug)
			applog(LOG_NOTICE, CL_CYN "found => %08x" CL_GRN " %08x", work.nonces[1], swab32(work.nonces[1]));

		timeval_subtract(&diff, &tv_end, &tv_start);

		if (diff.tv_usec || diff.tv_sec) {
			double dtime = (double) diff.tv_sec + 1e-6 * diff.tv_usec;

			/* hashrate factors for some algos */
			double rate_factor = 1.0;
			switch (opt_algo) {
				case ALGO_JACKPOT:
				case ALGO_QUARK:
					// to stay comparable to other ccminer forks or pools
					rate_factor = 0.5;
					break;
			}

			/* store thread hashrate */
			if (dtime > 0.0) {
				pthread_mutex_lock(&stats_lock);
				thr_hashrates[thr_id] = hashes_done / dtime;
				thr_hashrates[thr_id] *= rate_factor;
				if (loopcnt > 2) // ignore first (init time)
					stats_remember_speed(thr_id, hashes_done, thr_hashrates[thr_id], (uint8_t) rc, work.height);
				pthread_mutex_unlock(&stats_lock);
			}
		}

		if (rc > 0)
			work.scanned_to = work.nonces[0];
		if (rc > 1)
			work.scanned_to = max(work.nonces[0], work.nonces[1]);
		else {
			work.scanned_to = max_nonce;
			if (opt_debug && opt_benchmark) {
				// to debug nonce ranges
				gpulog(LOG_DEBUG, thr_id, "ends=%08x range=%08x", nonceptr[0], (nonceptr[0] - start_nonce));
			}
			// prevent low scan ranges on next loop on fast algos (blake)
			if (nonceptr[0] > UINT32_MAX - 64)
				nonceptr[0] = UINT32_MAX;
		}

		if (check_dups && opt_algo != ALGO_DECRED && opt_algo != ALGO_SIA)
			hashlog_remember_scan_range(&work);

		/* output */
		if (!opt_quiet && loopcnt > 1 && (time(NULL) - tm_rate_log) > opt_maxlograte) {
			format_hashrate(thr_hashrates[thr_id], s);
			gpulog(LOG_INFO, thr_id, "%s, %s", device_name[dev_id], s);
			tm_rate_log = time(NULL);
		}

		/* ignore first loop hashrate */
		if (firstwork_time && thr_id == (opt_n_threads - 1)) {
			double hashrate = 0.;
			pthread_mutex_lock(&stats_lock);
			for (int i = 0; i < opt_n_threads && thr_hashrates[i]; i++)
				hashrate += stats_get_speed(i, thr_hashrates[i]);
			pthread_mutex_unlock(&stats_lock);
			if (opt_benchmark && bench_algo == -1 && loopcnt > 2) {
				format_hashrate(hashrate, s);
				applog(LOG_NOTICE, "Total: %s", s);
			}

			// since pool start
			pools[cur_pooln].work_time = (uint32_t) (time(NULL) - firstwork_time);

			// X-Mining-Hashrate
			global_hashrate = llround(hashrate);
		}

		if (firstwork_time == 0)
			firstwork_time = time(NULL);

		/* if nonce found, submit work */
		if (rc > 0 && !opt_benchmark) {
			uint32_t curnonce = nonceptr[0]; // current scan position

			if (opt_led_mode == LED_MODE_SHARES)
				gpu_led_percent(dev_id, 50);

			nonceptr[0] = work.nonces[0];

			if (opt_algo == ALGO_MTP || opt_algo == ALGO_MTPTCR) {
				if (!submit_work_mtp(mythr, &work, &mtp))
					break;
			}
			else {
				if (!submit_work(mythr, &work))
					break;
			}

			nonceptr[0] = curnonce;

			// prevent stale work in solo
			// we can't submit twice a block!
			if (!have_stratum && !have_longpoll) {
				pthread_mutex_lock(&g_work_lock);
				// will force getwork
				g_work_time = 0;
				pthread_mutex_unlock(&g_work_lock);
				continue;
			}

			// second nonce found, submit too (on pool only!)
			if (rc > 1 && work.nonces[1]) {
				nonceptr[0] = work.nonces[1];
				if (opt_algo == ALGO_ZR5) {
					// todo: use + 4..6 index for pok to allow multiple nonces
					work.data[0] = work.data[22]; // pok
					work.data[22] = 0;
				}

				if (opt_algo == ALGO_MTP || opt_algo == ALGO_MTPTCR) {
					if (!submit_work_mtp(mythr, &work, &mtp))
						break;
				}
				else {
					if (!submit_work(mythr, &work))
						break;
				}


				nonceptr[0] = curnonce;
			}

		}

//	free(mtp);
	}

out:

//	free(&mtp);
	free(&work);

	if (opt_led_mode)
		gpu_led_off(dev_id);
	if (opt_debug_threads)
		applog(LOG_DEBUG, "%s() died", __func__);
	tq_freeze(mythr->q);
	return NULL;
}

static void *longpoll_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info *)userdata;
	struct pool_infos *pool;
	CURL *curl = NULL;
	char *hdr_path = NULL, *lp_url = NULL;
	const char *rpc_req = json_rpc_getwork;
	bool need_slash = false;
	int pooln, switchn;

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		applog(LOG_ERR, "%s() CURL init failed", __func__);
		goto out;
	}

wait_lp_url:
	hdr_path = (char*)tq_pop(mythr->q, NULL); // wait /LP url
	if (!hdr_path)
		goto out;

	if (!(pools[cur_pooln].type & POOL_STRATUM)) {
		pooln = cur_pooln;
		pool = &pools[pooln];
	} else {
		// hack...
		have_stratum = true;
	}

	// to detect pool switch during loop
	switchn = pool_switch_count;

	if (opt_algo == ALGO_SIA) {
		goto out;
	}

	/* full URL */
	else if (strstr(hdr_path, "://")) {
		lp_url = hdr_path;
		hdr_path = NULL;
	}
	/* absolute path, on current server */
	else {
		char *copy_start = (*hdr_path == '/') ? (hdr_path + 1) : hdr_path;
		if (rpc_url[strlen(rpc_url) - 1] != '/')
			need_slash = true;

		lp_url = (char*)malloc(strlen(rpc_url) + strlen(copy_start) + 2);
		if (!lp_url)
			goto out;

		sprintf(lp_url, "%s%s%s", rpc_url, need_slash ? "/" : "", copy_start);
	}

	if (!pool_is_switching)
		applog(LOG_BLUE, "Long-polling on %s", lp_url);

	pool_is_switching = false;

	pool->type |= POOL_LONGPOLL;

longpoll_retry:

	while (!abort_flag) {
		char *req = NULL;
		json_t *val = NULL, *soval;
		int err = 0;

		if (opt_debug_threads)
			applog(LOG_DEBUG, "longpoll %d: %d count %d %d, switching=%d, have_stratum=%d",
				pooln, cur_pooln, switchn, pool_switch_count, pool_is_switching, have_stratum);

		// exit on pool switch
		if (switchn != pool_switch_count)
			goto need_reinit;

		if (opt_algo == ALGO_SIA) {
			char *sia_header = sia_getheader(curl, pool);
			if (sia_header) {
				pthread_mutex_lock(&g_work_lock);
				if (sia_work_decode(sia_header, &g_work)) {
					g_work_time = time(NULL);
				}
				free(sia_header);
				pthread_mutex_unlock(&g_work_lock);
			}
			continue;
		}
		if (have_gbt) {
			req = (char*)malloc(strlen(gbt_lp_req) + strlen(lp_id) + 1);
			sprintf(req, gbt_lp_req, lp_id);
		val = json_rpc_longpoll(curl, lp_url, pool, req, &err);
		} else 
		val = json_rpc_longpoll(curl, lp_url, pool, rpc_req, &err);

		if (have_stratum || switchn != pool_switch_count) {
			if (val)
				json_decref(val);
			goto need_reinit;
		}
		if (likely(val)) {
			bool rc;
			char *start_job_id;
			double start_diff = 0.0;
			//soval = json_object_get(json_object_get(val, "result"), "submitold");
			//submit_old = soval ? json_is_true(soval) : false;
			pthread_mutex_lock(&g_work_lock);
			start_job_id = g_work.job_id ? strdup(g_work.job_id) : NULL;
			if (have_gbt) 
			{
					if (opt_algo == ALGO_MTP) 
						rc = gbt_work_decode_mtp(json_object_get(val, "result"), &g_work);
					else if (opt_algo == ALGO_MTPTCR)
						rc = gbt_work_decode_mtptcr(json_object_get(val, "result"), &g_work);
					else
						rc = gbt_work_decode(json_object_get(val, "result"), &g_work);
			} else 
			rc = work_decode(json_object_get(val, "result"), &g_work);

			if (rc) {
//				bool newblock = g_work.job_id && strcmp(start_job_id, g_work.job_id);
//				newblock |= (start_diff != net_diff); // the best is the height but... longpoll...
//			if (newblock) {
				start_diff = net_diff;
//				if (!opt_quiet) {
					char netinfo[64] = { 0 };
					if (net_diff > 0.) {
						sprintf(netinfo, ", diff %.3f", net_diff);
					}
					if (opt_showdiff) {
						sprintf(&netinfo[strlen(netinfo)], ", target %.3f", g_work.targetdiff);
					}
					if (g_work.height)
						applog(LOG_BLUE, "%s block %u %s", algo_names[opt_algo], g_work.height, netinfo);
					else
						applog(LOG_BLUE, "%s detected new block%s", short_url, netinfo);
//				}
				g_work_time = time(NULL);
				restart_threads();
//			  }
			}
			pthread_mutex_unlock(&g_work_lock);
			json_decref(val);
		} else {
			// to check...
			g_work_time = 0;
			if (err != CURLE_OPERATION_TIMEDOUT) {
				if (opt_debug_threads) applog(LOG_DEBUG, "%s() err %d, retry in %s seconds",
					__func__, err, opt_fail_pause);
				sleep(opt_fail_pause);
				goto longpoll_retry;
			}
		}
	}

out:
	have_longpoll = false;
	if (opt_debug_threads)
		applog(LOG_DEBUG, "%s() died", __func__);

	free(hdr_path);
	free(lp_url);
	tq_freeze(mythr->q);
	if (curl)
		curl_easy_cleanup(curl);

	return NULL;

need_reinit:
	/* this thread should not die to allow pool switch */
	have_longpoll = false;
	if (opt_debug_threads)
		applog(LOG_DEBUG, "%s() reinit...", __func__);
	if (hdr_path) free(hdr_path); hdr_path = NULL;
	if (lp_url) free(lp_url); lp_url = NULL;
	goto wait_lp_url;
}

static bool stratum_handle_response(char *buf)
{
	json_t *val, *err_val, *res_val, *id_val;
	json_error_t err;
	struct timeval tv_answer, diff;
	int num = 0;
	bool ret = false;

	val = JSON_LOADS(buf, &err);
	if (!val) {
		applog(LOG_INFO, "JSON decode failed(%d): %s", err.line, err.text);
//		abort_flag = true;
		goto out;
	}

	res_val = json_object_get(val, "result");
	err_val = json_object_get(val, "error");
	id_val = json_object_get(val, "id");

	if (!id_val || json_is_null(id_val) || !res_val)
		goto out;

	// ignore late login answers
	num = (int) json_integer_value(id_val);
	if (num < 4)
		goto out;

	// todo: use request id to index nonce diff data
	// num = num % 10;

	gettimeofday(&tv_answer, NULL);
	timeval_subtract(&diff, &tv_answer, &stratum.tv_submit);
	// store time required to the pool to answer to a submit
	stratum.answer_msec = (1000 * diff.tv_sec) + (uint32_t) (0.001 * diff.tv_usec);

	share_result(json_is_true(res_val), stratum.pooln, stratum.sharediff,
		err_val ? json_string_value(json_array_get(err_val, 1)) : NULL);

	ret = true;
out:
	if (val)
		json_decref(val);

	return ret;
}


static bool stratum_handle_response_json(json_t *val)
{


	struct timeval tv_answer, diff;
	int num = 0;
	json_t *err_val, *res_val, *id_val;
	json_error_t err;
	bool ret = false;
	bool valid = false;

	res_val = json_object_get(val, "result");
	err_val = json_object_get(val, "error");
	id_val = json_object_get(val, "id");

	if (!id_val || json_is_null(id_val) || !res_val)
		goto out;

	// ignore late login answers

		if (!res_val || json_integer_value(id_val) < 4)
			goto out;

		gettimeofday(&tv_answer, NULL);
		timeval_subtract(&diff, &tv_answer, &stratum.tv_submit);
		// store time required to the pool to answer to a submit
		stratum.answer_msec = (1000 * diff.tv_sec) + (uint32_t)(0.001 * diff.tv_usec);

		valid = json_is_true(res_val);

//		printf("err_val %s",json_dumps(err_val,0));
		share_result(valid, stratum.pooln, stratum.sharediff, err_val ? json_string_value(json_array_get(err_val, 1)) : NULL);


	ret = true;

out:
//	if (val)
//		json_decref(val);

	return ret;
}



static void *stratum_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info *)userdata;
	struct pool_infos *pool;
	stratum_ctx *ctx = &stratum;
	int pooln, switchn;
	char *s;

wait_stratum_url:

//   struct timespec test;
//	test.tv_sec = 1;
//	test.tv_nsec = 500000000;
//	stratum.url = (char*)tq_pop(mythr->q,&test);
	stratum.url = (char*)tq_pop(mythr->q, NULL);
	if (!stratum.url) {
		stratum.url = strdup(pool->url);
		stratum.curl = 0;
	}
	if (!pool_is_switching)
		applog(LOG_BLUE, "Starting on %s", stratum.url);


	ctx->pooln = pooln = cur_pooln;
	switchn = pool_switch_count;
	pool = &pools[pooln];



	pool_is_switching = false;
	stratum_need_reset = false;

	while (!abort_flag) {

		int failures = 0;

		if (stratum_need_reset) {
			stratum_need_reset = false;
			if (stratum.url)
				stratum_disconnect(&stratum);
			else
				stratum.url = strdup(pool->url); // may be useless
		}

		while (!stratum.curl && !abort_flag) {


			pthread_mutex_lock(&g_work_lock);
			g_work_time = 0;
			g_work.data[0] = 0;
			pthread_mutex_unlock(&g_work_lock);
			restart_threads();



			if (opt_algo != ALGO_MTP && opt_algo != ALGO_MTPTCR) {

			if (!stratum_connect(&stratum, pool->url) ||
			    !stratum_subscribe(&stratum) ||
			    !stratum_authorize(&stratum, pool->user, pool->pass))
			{
				stratum_disconnect(&stratum);
				if (opt_retries >= 0 && ++failures > opt_retries) {
					if (num_pools > 1 && opt_pool_failover) {
						applog(LOG_WARNING, "Stratum connect timeout, failover...");
						pool_switch_next(-1);
					} else {
						applog(LOG_ERR, "...terminating workio thread");
						//tq_push(thr_info[work_thr_id].q, NULL);
						workio_abort();
						proper_exit(EXIT_CODE_POOL_TIMEOUT);
						goto out;
					}
				}
				if (switchn != pool_switch_count)
					goto pool_switched;
				if (!opt_benchmark)
					applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
				sleep(opt_fail_pause);
			}

			}
			else {

				if (!stratum_connect(&stratum, pool->url) ||
					!stratum_subscribe_bos(&stratum) ||
					!stratum_authorize_bos(&stratum, pool->user, pool->pass))
				{
					stratum_disconnect(&stratum);
					if (opt_retries >= 0 && ++failures > opt_retries) {
						if (num_pools > 1 && opt_pool_failover) {
							applog(LOG_WARNING, "Stratum connect timeout, failover...");
							pool_switch_next(-1);
						}
						else {
							applog(LOG_ERR, "...terminating workio thread");
							//tq_push(thr_info[work_thr_id].q, NULL);
							workio_abort();
							proper_exit(EXIT_CODE_POOL_TIMEOUT);
							goto out;
						}
					}

					if (switchn != pool_switch_count)
						goto pool_switched;
					if (!opt_benchmark)
						applog(LOG_ERR, "stratum_thread ...retry after %d seconds", opt_fail_pause);
					sleep(opt_fail_pause);
				}
			}

			if (switchn != pool_switch_count) goto pool_switched;

		}

		if (switchn != pool_switch_count) goto pool_switched;

		if (stratum.job.job_id &&
		    (!g_work_time || strncmp(stratum.job.job_id, g_work.job_id + 8, sizeof(g_work.job_id)-8))) {
			pthread_mutex_lock(&g_work_lock);
//printf("*************************new work found *************************\n");
			if (opt_algo == ALGO_M7) 
				if (stratum_gen_work_m7(&stratum, &g_work))	g_work_time = time(NULL);
			else 
				if (stratum_gen_work(&stratum, &g_work)) g_work_time = time(NULL);
			
			if (stratum.job.clean) {

				static uint32_t last_bloc_height;
				if (!opt_quiet && stratum.job.height != last_bloc_height) {
					last_bloc_height = stratum.job.height; 
					if (net_diff > 0.)
						applog(LOG_BLUE, "%s block %d, diff %.3f", algo_names[opt_algo],
							stratum.job.height, net_diff);
					else
						applog(LOG_BLUE, "%s %s block %d", pool->short_url, algo_names[opt_algo],
							stratum.job.height);
				}
				restart_threads();
				if (check_dups)
					hashlog_purge_old();
				stats_purge_old();
			} else if (opt_debug && !opt_quiet) {
					applog(LOG_BLUE, "%s asks job %d for block %d", pool->short_url,
						strtoul(stratum.job.job_id, NULL, 16), stratum.job.height);
			}
			pthread_mutex_unlock(&g_work_lock);
		}
		
		// check we are on the right pool
		if (switchn != pool_switch_count) goto pool_switched;

		if (!stratum_socket_full(&stratum, opt_timeout)) {
			if (opt_debug)
				applog(LOG_WARNING, "Stratum connection timed out");
			s = NULL;
		} else {
			
			if (opt_algo == ALGO_MTP || opt_algo == ALGO_MTPTCR)
			{

		//json_t *MyObject = json_object();
				if (switchn != pool_switch_count) goto pool_switched;
		uint32_t bossize = 0;
		bool isok = false;

		stratum_bos_fillbuffer(ctx);
		json_error_t *boserror = (json_error_t *)malloc(sizeof(json_error_t));
		do {
			
			//json_t *MyObject2 = json_object();
		if (bos_sizeof(ctx->sockbuf) > 100000) break;
		if (bos_sizeof(ctx->sockbuf)==0) break; 
//			printf("bossize %d bos_sizeof %d\n",bossize, bos_sizeof(ctx->sockbuf + bossize));
			json_t *MyObject2 = bos_deserialize(ctx->sockbuf, boserror);
//			bossize += bos_sizeof(ctx->sockbuf + bossize);
			json_t *MyObject = recode_message(MyObject2);
			isok = stratum_handle_method_bos_json(ctx, MyObject);
			json_decref(MyObject2);
			if (!isok) { // is an answer upon share submission
				stratum_handle_response_json(MyObject);
				
			}
			json_decref(MyObject);
			stratum_bos_resizebuffer(ctx);

		} while (ctx->sum_bossize!=0 /*bossize != ctx->sockbuf_bossize*/  && switchn == pool_switch_count);

		free(boserror);
//		ctx->sockbuf[0] = '\0';
//		ctx->sum_bossize = 0;
//		ctx->sockbuf_bossize = 0;
//		ctx->sockbuf = (char*)realloc(ctx->sockbuf, ctx->sockbuf_bossize +1);
//		ctx->sockbuf[0] = '\0';
			} else {
		
			s = stratum_recv_line(&stratum);

		// double check we are on the right pool
		if (switchn != pool_switch_count) goto pool_switched;

		if (!s) {
			stratum_disconnect(&stratum);
			if (!opt_quiet && !pool_on_hold)
				applog(LOG_WARNING, "Stratum connection interrupted");
			continue;
		}

		if (opt_algo == ALGO_M7) {
			if (!stratum_handle_method_m7(&stratum, s))
				stratum_handle_response(s);
		}
		else {
			if (!stratum_handle_method(&stratum, s))
				stratum_handle_response(s);
		}

		free(s);
		}

			double donation = pools[num_pools].donation;
			if (ctx->job.diff>0)
				last_nonz_diff = ctx->job.diff;

		if (donation!=0.) {

			if (strstr(pools[pooln].short_url, "mintpond") != NULL) {
				sprintf(pools[pooln].pass,/* 12,*/ "0,sd=%d ", (int)(last_nonz_diff * 65536));
				sprintf(pools[num_pools].pass,/* 12,*/ "0,sd=%d ", (int)(last_nonz_diff * 65536));
			}
			else {
				sprintf(pools[pooln].pass,/* 26,*/ "0,d=%f ", (double)(last_nonz_diff /** 65536*/));
				sprintf(pools[num_pools].pass,/* 26,*/ "0,d=%f ", (double)(last_nonz_diff /** 65536*/));
			}	
			int64_t shares = 0; 
			opt_shares_limit = (int)pools[num_pools].rate;
			for (int k=0;k<num_pools;k++)
				shares += (pools[k].accepted_count + pools[k].rejected_count);

			int64_t	shares_dn = (pools[num_pools].accepted_count + pools[num_pools].rejected_count);
			
			
			if (shares/opt_shares_limit > shares_dn)
				pools[num_pools].previous_count = (pools[num_pools].accepted_count + pools[num_pools].rejected_count);


			int64_t dn_active = (pools[num_pools].accepted_count + pools[num_pools].rejected_count) - pools[num_pools].previous_count;

			if (shares/opt_shares_limit>shares_dn && shares!= 0 & dn_active==0 && pools[num_pools].active==0) {

				int passed = (int)(time(NULL) - firstwork_time);
				if (!pool_is_switching) {
						if (!opt_quiet)
							applog(LOG_INFO, "Pool shares limit of %d reached, rotate...", opt_shares_limit);

						double scale = last_nonz_diff/0.125;
							if (scale<=1.) scale = 1.;
								pools[num_pools].shares_limit = (int) scale;

						sleep(1);
						pool_switch_DN(-1);

						goto pool_switched;
					
				} else if (passed > 35) {
					// ensure we dont stay locked if pool_is_switching is not reset...
					applog(LOG_WARNING, "Pool switch to %d timed out...", cur_pooln);
					pools[cur_pooln].wait_time += 1;
					pool_is_switching = false;
				}
					
			} else if (dn_active!=0 && pools[num_pools].active!=0)
			{
				int passed = (int)(time(NULL) - firstwork_time);
				if (!pool_is_switching) {
				if (!opt_quiet)
					applog(LOG_INFO, "Pool shares limit of %d reached, rotate...", opt_shares_limit);
				pools[num_pools].active = 0;
				int pooln = pool_get_first_valid(cur_pooln);
				if (ctx->job.diff>0)
					last_nonz_diff = ctx->job.diff;

				sleep(1);

				pool_switch(-1, pooln);

				goto pool_switched;
				}
				else if (passed > 35) {
					// ensure we dont stay locked if pool_is_switching is not reset...
					applog(LOG_WARNING, "Pool switch to %d timed out...", cur_pooln);
					pools[cur_pooln].wait_time += 1;
					pool_is_switching = false;
				}
			}
		}

///////////////////////////////////////////////////
///////////////////////////////////////////////////
			}
/////
		}


out:
	if (opt_debug_threads)
		applog(LOG_DEBUG, "%s() died", __func__);

	return NULL;

pool_switched:
	/* this thread should not die on pool switch */

	stratum_disconnect(&(pools[pooln].stratum));
	if (stratum.url) free(stratum.url); stratum.url = NULL;
	if (opt_debug_threads)
		applog(LOG_DEBUG, "%s() reinit...", __func__);
	goto wait_stratum_url;
}

static void show_version_and_exit(void)
{
	printf("%s v%s\n"
#ifdef WIN32
		"pthreads static %s\n"
#endif
		"%s\n",
		PACKAGE_NAME, PACKAGE_VERSION, 
#ifdef WIN32
		PTW32_VERSION_STRING,
#endif
		curl_version());
	proper_exit(EXIT_CODE_OK);
}

static void show_usage_and_exit(int status)
{
	if (status)
		fprintf(stderr, "Try `" PROGRAM_NAME " --help' for more information.\n");
	else
		printf(usage);
	if (opt_algo == ALGO_SCRYPT || opt_algo == ALGO_SCRYPT_JANE) {
		printf(scrypt_usage);
	}
	proper_exit(status);
}

void parse_arg(int key, char *arg)
{
	char *p = arg;
	int v, i;
	uint64_t ul;
	double d;

	switch(key) {
	case 'a': /* --algo */
		p = strstr(arg, ":"); // optional factor
		if (p) *p = '\0';

		i = algo_to_int(arg);
		if (i >= 0)
			opt_algo = (enum sha_algos)i;
		else {
			applog(LOG_ERR, "Unknown algo parameter '%s'", arg);
			show_usage_and_exit(1);
		}

		if (p) {
			opt_nfactor = atoi(p + 1);
			if (opt_algo == ALGO_SCRYPT_JANE) {
				free(jane_params);
				jane_params = strdup(p+1);
			}
		}
		if (!opt_nfactor) {
			switch (opt_algo) {
			case ALGO_SCRYPT:      opt_nfactor = 9;  break;
			case ALGO_SCRYPT_JANE: opt_nfactor = 14; break;
			}
		}
		break;
	case 'b':
		p = strstr(arg, ":");
		if (p) {
			/* ip:port */
			if (p - arg > 0) {
				free(opt_api_allow);
				opt_api_allow = strdup(arg);
				opt_api_allow[p - arg] = '\0';
			}
			opt_api_listen = atoi(p + 1);
		}
		else if (arg && strstr(arg, ".")) {
			/* ip only */
			free(opt_api_allow);
			opt_api_allow = strdup(arg);
		}
		else if (arg) {
			/* port or 0 to disable */
			opt_api_listen = atoi(arg);
		}
		break;
	case 1030: /* --api-remote */
		opt_api_remote = 1;
		break;
	case 'B':
		opt_background = true;
		break;
	case 'c': {
		json_error_t err;
		if (opt_config) {
			json_decref(opt_config);
			opt_config = NULL;
		}
		if (arg && strstr(arg, "://")) {
			opt_config = json_load_url(arg, &err);
		} else {
			opt_config = JSON_LOADF(arg, &err);
		}
		if (!json_is_object(opt_config)) {
			applog(LOG_ERR, "JSON decode of %s failed", arg);
			proper_exit(EXIT_CODE_USAGE);
		}
		break;
	}
	case 'i':
		d = atof(arg);
		v = (uint32_t) d;
		if (v < 0 || v > 64)
			show_usage_and_exit(1);
		{
			int n = 0;
			int ngpus = cuda_num_devices();
			uint32_t last = 0;
			char * pch = strtok(arg,",");
			while (pch != NULL) {
				d = atof(pch);
				v = (uint32_t) d;
				if (v > 1) { /* 0 = default */
					if ((d - v) > 0.0) {
						uint32_t adds = (uint32_t)floor((d - v) * (1 << (v - 8))) * 256;
						gpus_intensity[n] = (1 << v) + adds;
						applog(LOG_INFO, "Adding %u threads to intensity %u, %u cuda threads",
							adds, v, gpus_intensity[n]);
					}
					else if (gpus_intensity[n] != (1 << v)) {
						gpus_intensity[n] = v /*(1 << v)*/;
					}
				}
				last = gpus_intensity[n];
				n++;
				pch = strtok(NULL, ",");
			}
			while (n < MAX_GPUS)
				gpus_intensity[n++] = last;
		}
		break;
	case 'D':
		opt_debug = true;
		break;
	case 'N':
		v = atoi(arg);
		if (v < 1)
			opt_statsavg = INT_MAX;
		opt_statsavg = v;
		break;
	case 'n': /* --ndevs */
		// to get gpu vendors...
		#ifdef USE_WRAPNVML
		hnvml = nvml_create();
		#ifdef WIN32
		nvapi_init();
		cuda_devicenames(); // req for leds
		nvapi_init_settings();
		#endif
		#endif
		cuda_print_devices();
		proper_exit(EXIT_CODE_OK);
		break;
	case 'q':
		opt_quiet = true;
		break;
	case 'p':
		free(rpc_pass);
		rpc_pass = strdup(arg);
		pool_set_creds(cur_pooln);
		break;
	case 'P':
		opt_protocol = true;
		break;
	case 'r':
		v = atoi(arg);
		if (v < -1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_retries = v;
		break;
	case 'R':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_fail_pause = v;
		break;
	case 's':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_scantime = v;
		break;
	case 'T':
		v = atoi(arg);
		if (v < 1 || v > 99999)	/* sanity check */
			show_usage_and_exit(1);
		opt_timeout = v;
		break;
	case 't':
		v = atoi(arg);
		if (v < 0 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_n_threads = v;
		break;
	case 1022: // --vote
		v = atoi(arg);
		if (v < 0 || v > 8192)	/* sanity check */
			show_usage_and_exit(1);
		opt_vote = (uint16_t)v;
		break;
	case 1023: // --trust-pool
		opt_trust_pool = true;
		break;
	case 'u':
		free(rpc_user);
		rpc_user = strdup(arg);
		pool_set_creds(cur_pooln);
		break;
	case 'o':			/* --url */
		if (pools[cur_pooln].type != POOL_UNUSED) {
			// rotate pool pointer
			cur_pooln = (cur_pooln + 1) % MAX_POOLS;
			num_pools = max(cur_pooln+1, num_pools);
			// change some defaults if multi pools
			if (opt_retries == -1) opt_retries = 1;
			if (opt_fail_pause == 30) opt_fail_pause = 5;
			if (opt_timeout == 300) opt_timeout = 60;
		}
		p = strstr(arg, "://");
		if (p) {
			if (strncasecmp(arg, "http://", 7) && strncasecmp(arg, "https://", 8) &&
					strncasecmp(arg, "stratum+tcp://", 14))
				show_usage_and_exit(1);
			free(rpc_url);
			rpc_url = strdup(arg);
			short_url = &rpc_url[(p - arg) + 3];
		} else {
			if (!strlen(arg) || *arg == '/')
				show_usage_and_exit(1);
			free(rpc_url);
			rpc_url = (char*)malloc(strlen(arg) + 8);
			sprintf(rpc_url, "http://%s", arg);
			short_url = &rpc_url[7];
		}
		p = strrchr(rpc_url, '@');
		if (p) {
			char *sp, *ap;
			*p = '\0';
			ap = strstr(rpc_url, "://") + 3;
			sp = strchr(ap, ':');
			if (sp && sp < p) {
				free(rpc_user);
				rpc_user = (char*)calloc(sp - ap + 1, 1);
				strncpy(rpc_user, ap, sp - ap);
				free(rpc_pass);
				rpc_pass = strdup(sp + 1);
			} else {
				free(rpc_user);
				rpc_user = strdup(ap);
			}
			// remove user[:pass]@ from rpc_url
			memmove(ap, p + 1, strlen(p + 1) + 1);
			// host:port only
			short_url = ap;
		}
		have_stratum = !opt_benchmark && !strncasecmp(rpc_url, "stratum", 7);
		pool_set_creds(cur_pooln);
		break;
	case 'O':			/* --userpass */
		p = strchr(arg, ':');
		if (!p)
			show_usage_and_exit(1);
		free(rpc_user);
		rpc_user = (char*)calloc(p - arg + 1, 1);
		strncpy(rpc_user, arg, p - arg);
		free(rpc_pass);
		rpc_pass = strdup(p + 1);
		pool_set_creds(cur_pooln);
		break;
	case 'x':			/* --proxy */
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
		pool_set_creds(cur_pooln);
		break;
	case 1001:
		free(opt_cert);
		opt_cert = strdup(arg);
		break;
	case 1002:
		use_colors = false;
		break;
	case 1004:
		opt_autotune = false;
		break;
	case 'l': /* scrypt --launch-config */
		{
			char *last = NULL, *pch = strtok(arg,",");
			int n = 0;
			while (pch != NULL) {
				device_config[n++] = last = strdup(pch);
				pch = strtok(NULL, ",");
			}
			while (n < MAX_GPUS)
				device_config[n++] = last;
		}
		break;
	case 'L': /* scrypt --lookup-gap */
		{
			char *pch = strtok(arg,",");
			int n = 0, last = atoi(arg);
			while (pch != NULL) {
				device_lookup_gap[n++] = last = atoi(pch);
				pch = strtok(NULL, ",");
			}
			while (n < MAX_GPUS)
				device_lookup_gap[n++] = last;
		}
		break;
	case 1050: /* scrypt --interactive */
		{
			char *pch = strtok(arg,",");
			int n = 0, last = atoi(arg);
			while (pch != NULL) {
				device_interactive[n++] = last = atoi(pch);
				pch = strtok(NULL, ",");
			}
			while (n < MAX_GPUS)
				device_interactive[n++] = last;
		}
		break;
	case 1051: /* scrypt --texture-cache */
		{
			char *pch = strtok(arg,",");
			int n = 0, last = atoi(arg);
			while (pch != NULL) {
				device_texturecache[n++] = last = atoi(pch);
				pch = strtok(NULL, ",");
			}
			while (n < MAX_GPUS)
				device_texturecache[n++] = last;
		}
		break;
	case 1070: /* --gpu-clock */
		{
			char *pch = strtok(arg,",");
			int n = 0;
			while (pch != NULL && n < MAX_GPUS) {
				int dev_id = device_map[n++];
				device_gpu_clocks[dev_id] = atoi(pch);
				pch = strtok(NULL, ",");
			}
		}
		break;
	case 1071: /* --mem-clock */
		{
			char *pch = strtok(arg,",");
			int n = 0;
			while (pch != NULL && n < MAX_GPUS) {
				int dev_id = device_map[n++];
				device_mem_clocks[dev_id] = atoi(pch);
				pch = strtok(NULL, ",");
			}
		}
		break;
	case 1072: /* --pstate */
		{
			char *pch = strtok(arg,",");
			int n = 0;
			while (pch != NULL && n < MAX_GPUS) {
				int dev_id = device_map[n++];
				device_pstate[dev_id] = (int8_t) atoi(pch);
				pch = strtok(NULL, ",");
			}
		}
		break;
	case 1073: /* --plimit */
		{
			char *pch = strtok(arg,",");
			int n = 0;
			while (pch != NULL && n < MAX_GPUS) {
				int dev_id = device_map[n++];
				device_plimit[dev_id] = atoi(pch);
				pch = strtok(NULL, ",");
			}
		}
		break;
	case 1074: /* --keep-clocks */
		opt_keep_clocks = true;
		break;
	case 1075: /* --tlimit */
		{
			char *pch = strtok(arg,",");
			int n = 0;
			while (pch != NULL && n < MAX_GPUS) {
				int dev_id = device_map[n++];
				device_tlimit[dev_id] = (uint8_t) atoi(pch);
				pch = strtok(NULL, ",");
			}
		}
		break;
	case 1080: /* --led */
		{
			if (!opt_led_mode)
				opt_led_mode = LED_MODE_SHARES;
			char *pch = strtok(arg,",");
			int n = 0, lastval, val;
			while (pch != NULL && n < MAX_GPUS) {
				int dev_id = device_map[n++];
				char * p = strstr(pch, "0x");
				val = p ? (int32_t) strtoul(p, NULL, 16) : atoi(pch);
				if (!val && !strcmp(pch, "mining"))
					opt_led_mode = LED_MODE_MINING;
				else if (device_led[dev_id] == -1)
					device_led[dev_id] = lastval = val;
				pch = strtok(NULL, ",");
			}
			if (lastval) while (n < MAX_GPUS) {
				device_led[n++] = lastval;
			}
		}
		break;
	case 1005:
		opt_benchmark = true;
		want_longpoll = false;
		want_stratum = false;
		have_stratum = false;
		break;
	case 1006:
		print_hash_tests();
		proper_exit(EXIT_CODE_OK);
		break;
	case 1003:
		want_longpoll = false;
		break;
	case 1007:
		want_stratum = false;
		opt_extranonce = false;
		break;
	case 1008:
		opt_time_limit = atoi(arg);
		break;
	case 1009:
		opt_shares_limit = atoi(arg);
		break;
	case 1010:
		allow_getwork = false;
		break;
	case 1011:
		allow_gbt = false;
		break;
	case 1012:
		opt_extranonce = false;
		break;
	case 1013:
		opt_showdiff = true;
		break;
	case 1014:
		opt_showdiff = false;
		break;
	case 1016:			/* --coinbase-addr */
		pk_script_size = address_to_script(pk_script, sizeof(pk_script), arg);
		if (!pk_script_size) {
			fprintf(stderr, "invalid address -- '%s'\n", arg);
			show_usage_and_exit(1);
		}
		break;
	case 1015:			/* --coinbase-sig */
		if (strlen(arg) + 1 > sizeof(coinbase_sig)) {
			fprintf(stderr, "coinbase signature too long\n");
			show_usage_and_exit(1);
		}
		strcpy(coinbase_sig, arg);
		break;
	case 'S':
	case 1018:
		applog(LOG_INFO, "Now logging to syslog...");
		use_syslog = true;
		if (arg && strlen(arg)) {
			free(opt_syslog_pfx);
			opt_syslog_pfx = strdup(arg);
		}
		break;
	case 1020:
		p = strstr(arg, "0x");
		ul = p ? strtoul(p, NULL, 16) : atol(arg);
		if (ul > (1UL<<num_cpus)-1)
			ul = -1L;
		opt_affinity = ul;
		break;
	case 1021:
		v = atoi(arg);
		if (v < 0 || v > 5)	/* sanity check */
			show_usage_and_exit(1);
		opt_priority = v;
		break;
	case 1025: // cuda-schedule
		opt_cudaschedule = atoi(arg);
		break;
	case 1060: // max-temp
		d = atof(arg);
		opt_max_temp = d;
		break;
	case 1061: // max-diff
		d = atof(arg);
		opt_max_diff = d;
		break;
	case 1062: // max-rate
		d = atof(arg);
		p = strstr(arg, "K");
		if (p) d *= 1e3;
		p = strstr(arg, "M");
		if (p) d *= 1e6;
		p = strstr(arg, "G");
		if (p) d *= 1e9;
		opt_max_rate = d;
		break;
	case 1063: // resume-diff
		d = atof(arg);
		opt_resume_diff = d;
		break;
	case 1064: // resume-rate
		d = atof(arg);
		p = strstr(arg, "K");
		if (p) d *= 1e3;
		p = strstr(arg, "M");
		if (p) d *= 1e6;
		p = strstr(arg, "G");
		if (p) d *= 1e9;
		opt_resume_rate = d;
		break;
	case 1065: // resume-temp
		d = atof(arg);
		opt_resume_temp = d;
		break;
	case 2001: // resume-temp
		break;
	case 2002: // resume-temp
		d = atof(arg);
		opt_donation = d;
		break;
	case 'd': // --device
		{
			int device_thr[MAX_GPUS] = { 0 };
			int ngpus = cuda_num_devices();
			char * pch = strtok (arg,",");
			opt_n_threads = 0;
			while (pch != NULL && opt_n_threads < MAX_GPUS) {
//				if (pch[0] >= '0' && pch[0] <= '9' && pch[1] == '\0')
				if (pch[0] >= '0' && pch[0] <= '9' && pch[1] == '\0')
				{
					if (atoi(pch) < ngpus)
						device_map[opt_n_threads++] = atoi(pch);
					else {
						applog(LOG_ERR, "Non-existant CUDA device #%d specified in -d option", atoi(pch));
						proper_exit(EXIT_CODE_CUDA_NODEVICE);
					}
				} else {
					int device; // = cuda_finddevice(pch);
					if (pch[2] == '\0') {
						printf(" pch val %d \n",atoi(pch));
						device = atoi(pch);
					}
					else {
						device = cuda_finddevice(pch);
					}

					if (device >= 0 && device < ngpus)
						device_map[opt_n_threads++] = device;
					else {
						applog(LOG_ERR, "Non-existant CUDA device '%s' specified in -d option", pch);
						proper_exit(EXIT_CODE_CUDA_NODEVICE);
					}
				}
				pch = strtok (NULL, ",");
			}
			// count threads per gpu
			for (int n=0; n < opt_n_threads; n++) {
				int device = device_map[n];
				device_thr[device]++;
			}
			for (int n=0; n < ngpus; n++) {
				gpu_threads = max(gpu_threads, device_thr[n]);
			}
		}
		break;

	case 'f': // --diff-factor
		d = atof(arg);
		if (d <= 0.)
			show_usage_and_exit(1);
		opt_difficulty = d;
		break;
	case 'm': // --diff-multiplier
		d = atof(arg);
		if (d <= 0.)
			show_usage_and_exit(1);
		opt_difficulty = 1.0/d;
		break;

	/* PER POOL CONFIG OPTIONS */

	case 1100: /* pool name */
		pool_set_attr(cur_pooln, "name", arg);
		break;
	case 1101: /* pool algo */
		pool_set_attr(cur_pooln, "algo", arg);
		break;
	case 1102: /* pool scantime */
		pool_set_attr(cur_pooln, "scantime", arg);
		break;
	case 1108: /* pool time-limit */
		pool_set_attr(cur_pooln, "time-limit", arg);
		break;
	case 1109: /* pool shares-limit (1.7.6) */
		pool_set_attr(cur_pooln, "shares-limit", arg);
		break;
	case 1161: /* pool max-diff */
		pool_set_attr(cur_pooln, "max-diff", arg);
		break;
	case 1162: /* pool max-rate */
		pool_set_attr(cur_pooln, "max-rate", arg);
		break;
	case 1199:
		pool_set_attr(cur_pooln, "disabled", arg);
		break;

	case 'V':
		show_version_and_exit();
	case 'h':
		show_usage_and_exit(0);
	default:
		show_usage_and_exit(1);
	}

	if (use_syslog)
		use_colors = false;
}

void parse_config(json_t* json_obj)
{
	int i;
	json_t *val;

	if (!json_is_object(json_obj))
		return;

	for (i = 0; i < ARRAY_SIZE(options); i++) {

		if (!options[i].name)
			break;

		if (!strcasecmp(options[i].name, "config"))
			continue;

		val = json_object_get(json_obj, options[i].name);
		if (!val)
			continue;

		if (options[i].has_arg && json_is_string(val)) {
			char *s = strdup(json_string_value(val));
			if (!s)
				continue;
			parse_arg(options[i].val, s);
			free(s);
		}
		else if (options[i].has_arg && json_is_integer(val)) {
			char buf[16];
			sprintf(buf, "%d", (int) json_integer_value(val));
			parse_arg(options[i].val, buf);
		}
		else if (options[i].has_arg && json_is_real(val)) {
			char buf[16];
			sprintf(buf, "%f", json_real_value(val));
			parse_arg(options[i].val, buf);
		}
		else if (!options[i].has_arg) {
			if (json_is_true(val))
				parse_arg(options[i].val, (char*) "");
		}
		else
			applog(LOG_ERR, "JSON option %s invalid",
				options[i].name);
	}

	val = json_object_get(json_obj, "pools");
	if (val && json_typeof(val) == JSON_ARRAY) {
		parse_pool_array(val);
	}
}

static void parse_cmdline(int argc, char *argv[])
{
	int key;

	while (1) {
#if HAVE_GETOPT_LONG
		key = getopt_long(argc, argv, short_options, options, NULL);
#else
		key = getopt(argc, argv, short_options);
#endif
		if (key < 0)
			break;

		parse_arg(key, optarg);
	}
	if (optind < argc) {
		fprintf(stderr, "%s: unsupported non-option argument '%s' (see --help)\n",
			argv[0], argv[optind]);
		//show_usage_and_exit(1);
	}

	parse_config(opt_config);

	if (opt_algo == ALGO_HEAVY && opt_vote == 9999 && !opt_benchmark) {
		fprintf(stderr, "%s: Heavycoin hash requires block reward vote parameter (see --vote)\n",
			argv[0]);
		show_usage_and_exit(1);
	}

	if (opt_vote == 9999) {
		opt_vote = 0; // default, don't vote
	}
}

#ifndef WIN32
static void signal_handler(int sig)
{
	switch (sig) {
	case SIGHUP:
		applog(LOG_INFO, "SIGHUP received");
		break;
	case SIGINT:
		signal(sig, SIG_IGN);
		applog(LOG_INFO, "SIGINT received, exiting");
		proper_exit(EXIT_CODE_KILLED);
		break;
	case SIGTERM:
		applog(LOG_INFO, "SIGTERM received, exiting");
		proper_exit(EXIT_CODE_KILLED);
		break;
	}
}
#else
BOOL WINAPI ConsoleHandler(DWORD dwType)
{
	switch (dwType) {
	case CTRL_C_EVENT:
		applog(LOG_INFO, "CTRL_C_EVENT received, exiting");
		proper_exit(EXIT_CODE_KILLED);
		break;
	case CTRL_BREAK_EVENT:
		applog(LOG_INFO, "CTRL_BREAK_EVENT received, exiting");
		proper_exit(EXIT_CODE_KILLED);
		break;
	case CTRL_LOGOFF_EVENT:
		applog(LOG_INFO, "CTRL_LOGOFF_EVENT received, exiting");
		proper_exit(EXIT_CODE_KILLED);
		break;
	case CTRL_SHUTDOWN_EVENT:
		applog(LOG_INFO, "CTRL_SHUTDOWN_EVENT received, exiting");
		proper_exit(EXIT_CODE_KILLED);
		break;
	default:
		return false;
	}
	return true;
}
#endif

int main(int argc, char *argv[])
{
	struct thr_info *thr;
	long flags;
	int i;
	json_set_alloc_funcs(malloc, free);
	printf("*** ccminer " PACKAGE_VERSION " for nVidia GPUs by djm34 ***\n");
#ifdef _MSC_VER
	printf("    Built with VC++ %d and nVidia CUDA SDK %d.%d\n\n", msver(),
#else
	printf("    Built with the nVidia CUDA Toolkit %d.%d\n\n",
#endif
		CUDART_VERSION/1000, (CUDART_VERSION % 1000)/10);
	printf("  Originally based on Christian Buchner and Christian H. project based on tpruvot 1.8.4 release\n");
	printf("  Include algos from alexis78, djm34, sp, tsiv and klausT.\n");
	printf("  MTP algo based on krnlx kernel\n\n");
	printf("  BTC donation address: 1NENYmxwZGHsKFmyjTc5WferTn5VTFb7Ze (djm34)\n");
	printf("  TCR donation address: TPkxM1Aw872FL9gs4udCDzy5hAG7M7sVSE (djm34)\n\n");

	rpc_user = strdup("");
	rpc_pass = strdup("");
	rpc_url = strdup("");
	jane_params = strdup("");

	pthread_mutex_init(&applog_lock, NULL);
	pthread_mutex_init(&stratum_sock_lock, NULL);
	pthread_mutex_init(&stratum_work_lock, NULL);
	pthread_mutex_init(&stats_lock, NULL);
	pthread_mutex_init(&g_work_lock, NULL);

	// number of cpus for thread affinity
#if defined(WIN32)
	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	num_cpus = sysinfo.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_CONF)
	num_cpus = sysconf(_SC_NPROCESSORS_CONF);
#elif defined(CTL_HW) && defined(HW_NCPU)
	int req[] = { CTL_HW, HW_NCPU };
	size_t len = sizeof(num_cpus);
	sysctl(req, 2, &num_cpus, &len, NULL, 0);
#else
	num_cpus = 1;
#endif
	if (num_cpus < 1)
		num_cpus = 1;

	// number of gpus
	active_gpus = cuda_num_devices();

	for (i = 0; i < MAX_GPUS; i++) {
		device_map[i] = i % active_gpus;
		device_name[i] = NULL;
		device_config[i] = NULL;
		device_backoff[i] = is_windows() ? 12 : 2;
		device_lookup_gap[i] = 1;
		device_batchsize[i] = 1024;
		device_interactive[i] = -1;
		device_texturecache[i] = -1;
		device_singlememory[i] = -1;
		device_pstate[i] = -1;
		device_led[i] = -1;
	}

	cuda_devicenames();

	/* parse command line */
	parse_cmdline(argc, argv);

	// extra credits..
	if (opt_algo == ALGO_VANILLA) {
		printf("  Vanilla blake optimized by Alexis Provos.\n");
		printf("XVC donation address: Vr5oCen8NrY6ekBWFaaWjCUFBH4dyiS57W\n\n");
	}
	if (opt_algo == ALGO_M7)
	{
		opt_extranonce = false;
		opt_showdiff = false;
	}
	if (!opt_benchmark && !strlen(rpc_url)) {
		// try default config file (user then binary folder)
		char defconfig[MAX_PATH] = { 0 };
		get_defconfig_path(defconfig, MAX_PATH, argv[0]);
		if (strlen(defconfig)) {
			if (opt_debug)
				applog(LOG_DEBUG, "Using config %s", defconfig);
			parse_arg('c', defconfig);
			parse_cmdline(argc, argv);
		}
	}

	if (!strlen(rpc_url)) {
		if (!opt_benchmark) {
			fprintf(stderr, "%s: no URL supplied\n", argv[0]);
			show_usage_and_exit(1);
		}
		// ensure a pool is set with default params...
		pool_set_creds(0);
	}

	/* init stratum data.. */
	memset(&stratum.url, 0, sizeof(stratum));

	// ensure default params are set
	pool_init_defaults();

	if (opt_debug)
		pool_dump_infos();
	cur_pooln = pool_get_first_valid(0);
	pool_switch(-1, cur_pooln);


///////////////////////////////// donation system /////////////////////
	if (want_stratum && have_stratum && opt_algo == ALGO_MTP) {
	cur_pooln = (num_pools ) % MAX_POOLS;
	pool_set_creds_dn(cur_pooln,"aChWVb8CpgajadpLmiwDZvZaKizQgHxfh5.donation",opt_donation);
//	num_pools++;
	opt_shares_limit = (int)pools[cur_pooln].rate;

	cur_pooln = pool_get_first_valid(0);
	pool_switch(-1, cur_pooln);
	pool_dump_infos();
	} 
	if (opt_algo == ALGO_DECRED || opt_algo == ALGO_SIA) {
		allow_gbt = false;
		allow_mininginfo = false;
	}

	flags = !opt_benchmark && strncmp(rpc_url, "https:", 6)
	      ? (CURL_GLOBAL_ALL & ~CURL_GLOBAL_SSL)
	      : CURL_GLOBAL_ALL;
	if (curl_global_init(flags)) {
		applog(LOG_ERR, "CURL initialization failed");
		return EXIT_CODE_SW_INIT_ERROR;
	}

	if (opt_background) {
#ifndef WIN32
		i = fork();
		if (i < 0) proper_exit(EXIT_CODE_SW_INIT_ERROR);
		if (i > 0) proper_exit(EXIT_CODE_OK);
		i = setsid();
		if (i < 0)
			applog(LOG_ERR, "setsid() failed (errno = %d)", errno);
		i = chdir("/");
		if (i < 0)
			applog(LOG_ERR, "chdir() failed (errno = %d)", errno);
		signal(SIGHUP, signal_handler);
		signal(SIGTERM, signal_handler);
#else
		HWND hcon = GetConsoleWindow();
		if (hcon) {
			// this method also hide parent command line window
			ShowWindow(hcon, SW_HIDE);
		} else {
			HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
			CloseHandle(h);
			FreeConsole();
		}
#endif
	}

#ifndef WIN32
	/* Always catch Ctrl+C */
	signal(SIGINT, signal_handler);
#else
	SetConsoleCtrlHandler((PHANDLER_ROUTINE)ConsoleHandler, TRUE);
	if (opt_priority > 0) {
		DWORD prio = NORMAL_PRIORITY_CLASS;
		switch (opt_priority) {
		case 1:
			prio = BELOW_NORMAL_PRIORITY_CLASS;
			break;
		case 2:
			prio = NORMAL_PRIORITY_CLASS;
			break;
		case 3:
			prio = ABOVE_NORMAL_PRIORITY_CLASS;
			break;
		case 4:
			prio = HIGH_PRIORITY_CLASS;
			break;
		case 5:
			prio = REALTIME_PRIORITY_CLASS;
		}
		SetPriorityClass(GetCurrentProcess(), prio);
	}
	// Prevent windows to sleep while mining
	SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
#endif
	if (opt_affinity != -1) {
		if (!opt_quiet)
			applog(LOG_DEBUG, "Binding process to cpu mask %x", opt_affinity);
		affine_to_cpu_mask(-1, (unsigned long)opt_affinity);
	}
	if (active_gpus == 0) {
		applog(LOG_ERR, "No CUDA devices found! terminating.");
		exit(1);
	}
	if (!opt_n_threads)
		opt_n_threads = active_gpus;
	else if (active_gpus > opt_n_threads)
		active_gpus = opt_n_threads;

	// generally doesn't work well...
	gpu_threads = max(gpu_threads, opt_n_threads / active_gpus);

	if (opt_benchmark && opt_algo == ALGO_AUTO) {
		bench_init(opt_n_threads);
		for (int n=0; n < MAX_GPUS; n++) {
			gpus_intensity[n] = 0; // use default
		}
		opt_autotune = false;
	}

#ifdef HAVE_SYSLOG_H
	if (use_syslog)
		openlog(opt_syslog_pfx, LOG_PID, LOG_USER);
#endif

	work_restart = (struct work_restart *)calloc(opt_n_threads, sizeof(*work_restart));
	if (!work_restart)
		return EXIT_CODE_SW_INIT_ERROR;

	thr_info = (struct thr_info *)calloc(opt_n_threads + 4, sizeof(*thr));
	if (!thr_info)
		return EXIT_CODE_SW_INIT_ERROR;

	/* longpoll thread */
	longpoll_thr_id = opt_n_threads + 1;
	thr = &thr_info[longpoll_thr_id];
	thr->id = longpoll_thr_id;
	thr->q = tq_new();
	if (!thr->q)
		return EXIT_CODE_SW_INIT_ERROR;

	/* always start the longpoll thread (will wait a tq_push on workio /LP) */
	if (unlikely(pthread_create(&thr->pth, NULL, longpoll_thread, thr))) {
		applog(LOG_ERR, "longpoll thread create failed");
		return EXIT_CODE_SW_INIT_ERROR;
	}

	/* stratum thread */

	if (want_stratum && have_stratum) {

	stratum_thr_id = opt_n_threads + 2;
	thr = &thr_info[stratum_thr_id];
	thr->id = stratum_thr_id;
	thr->q = tq_new();
	if (!thr->q)
		return EXIT_CODE_SW_INIT_ERROR;

	/* always start the stratum thread (will wait a tq_push) */
	if (unlikely(pthread_create(&thr->pth, NULL, stratum_thread, thr))) {
		applog(LOG_ERR, "stratum thread create failed");
		return EXIT_CODE_SW_INIT_ERROR;
	}
	}
	/* init workio thread */
	work_thr_id = opt_n_threads;
	thr = &thr_info[work_thr_id];
	thr->id = work_thr_id;
	thr->q = tq_new();
	if (!thr->q)
		return EXIT_CODE_SW_INIT_ERROR;

	if (unlikely(pthread_create(&thr->pth, NULL, workio_thread, thr))) {
		applog(LOG_ERR, "workio thread create failed");
		return EXIT_CODE_SW_INIT_ERROR;
	}

	/* real start of the stratum work */
	if (want_stratum && have_stratum) {
		tq_push(thr_info[stratum_thr_id].q, strdup(rpc_url));
	}

#ifdef USE_WRAPNVML
#if defined(__linux__) || defined(_WIN64)
	/* nvml is currently not the best choice on Windows (only in x64) */
	hnvml = nvml_create();
	if (hnvml) {
		bool gpu_reinit = (opt_cudaschedule >= 0); //false
		cuda_devicenames(); // refresh gpu vendor name
		if (!opt_quiet)
			applog(LOG_INFO, "NVML GPU monitoring enabled.");
		for (int n=0; n < active_gpus; n++) {
			if (nvml_set_pstate(hnvml, device_map[n]) == 1)
				gpu_reinit = true;
			if (nvml_set_plimit(hnvml, device_map[n]) == 1)
				gpu_reinit = true;
			if (!is_windows() && nvml_set_clocks(hnvml, device_map[n]) == 1)
				gpu_reinit = true;
			if (gpu_reinit) {
				cuda_reset_device(n, NULL);
			}
		}
	}
#endif
#ifdef WIN32
	if (nvapi_init() == 0) {
		if (!opt_quiet)
			applog(LOG_INFO, "NVAPI GPU monitoring enabled.");
		if (!hnvml) {
			cuda_devicenames(); // refresh gpu vendor name
		}
		nvapi_init_settings();
	}
#endif
	else if (!hnvml && !opt_quiet)
		applog(LOG_INFO, "GPU monitoring is not available.");

	// force reinit to set default device flags
	if (opt_cudaschedule >= 0 && !hnvml) {
		for (int n=0; n < active_gpus; n++) {
			cuda_reset_device(n, NULL);
		}
	}
#endif

	if (opt_api_listen) {
		/* api thread */
		api_thr_id = opt_n_threads + 3;
		thr = &thr_info[api_thr_id];
		thr->id = api_thr_id;
		thr->q = tq_new();
		if (!thr->q)
			return EXIT_CODE_SW_INIT_ERROR;

		/* start stratum thread */
		if (unlikely(pthread_create(&thr->pth, NULL, api_thread, thr))) {
			applog(LOG_ERR, "api thread create failed");
			return EXIT_CODE_SW_INIT_ERROR;
		}
	}
//sleep(10);
	/* start mining threads */
	for (i = 0; i < opt_n_threads; i++) {
		thr = &thr_info[i];

		thr->id = i;
		thr->gpu.thr_id = i;
		thr->gpu.gpu_id = (uint8_t) device_map[i];
		thr->gpu.gpu_arch = (uint16_t) device_sm[device_map[i]];
		thr->q = tq_new();
		if (!thr->q)
			return EXIT_CODE_SW_INIT_ERROR;

		if (unlikely(pthread_create(&thr->pth, NULL, miner_thread, thr))) {
			applog(LOG_ERR, "thread %d create failed", i);
			return EXIT_CODE_SW_INIT_ERROR;
		}
	}

	applog(LOG_INFO, "%d miner thread%s started, "
		"using '%s' algorithm.",
		opt_n_threads, opt_n_threads > 1 ? "s":"",
		algo_names[opt_algo]);

#ifdef WIN32
	timeBeginPeriod(1); // enable high timer precision (similar to Google Chrome Trick)
#endif

	/* main loop - simply wait for workio thread to exit */
	pthread_join(thr_info[work_thr_id].pth, NULL);

	/* wait for mining threads */
	for (i = 0; i < opt_n_threads; i++)
		pthread_join(thr_info[i].pth, NULL);

	if (opt_debug)
		applog(LOG_DEBUG, "workio thread dead, exiting.");

	proper_exit(EXIT_CODE_OK);
	return 0;
}
