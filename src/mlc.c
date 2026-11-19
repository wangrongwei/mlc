#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <limits.h>

#include "helper.h"
#include "allocation.h"
#include "intrd.h"
#include "hittest.h"
#include "combined-loadlat.h"
#include "busythread.h"
#include "c2c_bandwidth.h"
#include "adv_scan.h"
#include "ddio.h"
#include "fastdelay_dispatch.h"
#include "delay_pointer_chase.h"

#define MLC_VERSION "v3.13"
#define DEFAULT_LATENCY_BUF_MB 200
#define DEFAULT_BW_BUF_PER_THREAD_MB 100
#define DEFAULT_TIME_SEC 2

/* Kernel NUMA Balancing State Management */
static int orig_numa_balancing = -1;
static int orig_scan_delay = -1;
static bool numa_balancing_modified = false;
static bool scan_delay_modified = false;

static int read_proc_int(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	int val = -1;
	if (fscanf(f, "%d", &val) != 1) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return val;
}

/* Async-signal-safe integer writer for /proc/sys/... restoration */
static int write_proc_int(const char *path, int val)
{
	int fd = open(path, O_WRONLY | O_TRUNC);
	if (fd < 0)
		return -1;

	char buf[32];
	char digits[16];
	int pos = 0;
	int n = 0;
	unsigned int u;

	if (val < 0) {
		buf[pos++] = '-';
		u = (unsigned int)(-(int64_t)val);
	} else {
		u = (unsigned int)val;
	}
	do {
		digits[n++] = (char)('0' + (u % 10U));
		u /= 10U;
	} while (u > 0U && n < (int)sizeof(digits));
	while (n > 0)
		buf[pos++] = digits[--n];
	buf[pos++] = '\n';

	ssize_t ret = write(fd, buf, (size_t)pos);
	close(fd);
	return (ret == (ssize_t)pos) ? 0 : -1;
}

static void restore_numa_balancing(void)
{
	if (numa_balancing_modified && orig_numa_balancing >= 0) {
		write_proc_int("/proc/sys/kernel/numa_balancing", orig_numa_balancing);
		numa_balancing_modified = false;
	}
	if (scan_delay_modified && orig_scan_delay >= 0) {
		write_proc_int("/proc/sys/kernel/numa_balancing_scan_delay_ms", orig_scan_delay);
		scan_delay_modified = false;
	}
}

static void manage_numa_balancing(void)
{
	orig_numa_balancing = read_proc_int("/proc/sys/kernel/numa_balancing");
	if (orig_numa_balancing >= 0) {
		if (orig_numa_balancing != 0 &&
		    write_proc_int("/proc/sys/kernel/numa_balancing", 0) == 0) {
			numa_balancing_modified = true;
		}
		return;
	}

	orig_scan_delay = read_proc_int("/proc/sys/kernel/numa_balancing_scan_delay_ms");
	if (orig_scan_delay >= 0) {
		if (write_proc_int("/proc/sys/kernel/numa_balancing_scan_delay_ms",
				   1000000) == 0) {
			scan_delay_modified = true;
		}
	}
}

static void mlc_exit(int status, const char *msg)
{
	if (msg && *msg) {
		fprintf(stderr, "%s\n", msg);
	}
	stop_pstate_threads();
	msr_restore_prefetchers();
	restore_numa_balancing();
	restore_1gb_hugepages();
	exit(status);
}

static void mlc_signal_handler(int sig)
{
	static const char exit_msg[] = "\nExiting...\n";
	signal(sig, SIG_IGN);
	msr_restore_prefetchers();
	restore_numa_balancing();
	restore_1gb_hugepages();
	ssize_t wr = write(STDERR_FILENO, exit_msg, sizeof(exit_msg) - 1);
	(void)wr;
	_exit(128 + sig);
}

static void setup_signal_handlers(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = mlc_signal_handler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGTSTP, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	atexit(restore_1gb_hugepages);
	atexit(restore_numa_balancing);
	atexit(msr_restore_prefetchers);
}

static void check_arg_has_val(char opt, const char *val_str)
{
	if (!val_str || *val_str == '\0') {
		fprintf(stderr,
			"Value missing for the -%c parameter or an unnecessary blank space between -%c and its value\nExiting..\n",
			opt, opt);
		mlc_exit(22, NULL);
	}
}

static void check_arg_no_val(char opt, const char *val_str)
{
	if (val_str && *val_str != '\0') {
		fprintf(stderr, "-%c parameter does not accept any value\nExiting..\n", opt);
		mlc_exit(22, NULL);
	}
}

static int64_t parse_num_with_unit(const char *s, int *has_unit)
{
	if (!s || *s == '\0') {
		fprintf(stderr, "Parameter value %s is not number\n", s ? s : "");
		mlc_exit(22, NULL);
	}

	size_t len = strlen(s);
	int shift = 0;
	int unit = 0;
	size_t digit_len = len;
	char last = s[len - 1];

	if (last == 'K' || last == 'k') {
		shift = 10;
		unit = 1;
		digit_len = len - 1;
	} else if (last == 'M' || last == 'm') {
		shift = 20;
		unit = 1;
		digit_len = len - 1;
	} else if (last == 'G' || last == 'g') {
		shift = 30;
		unit = 1;
		digit_len = len - 1;
	} else if (last < '0' || last > '9') {
		fprintf(stderr, "Parameter value %s is not number\n", s);
		mlc_exit(22, NULL);
	}

	if (digit_len == 0) {
		fprintf(stderr, "Parameter value %s is not number\n", s);
		mlc_exit(22, NULL);
	}

	for (size_t i = 0; i < digit_len; i++) {
		if (s[i] < '0' || s[i] > '9') {
			fprintf(stderr, "Parameter value %s is not number\n", s);
			mlc_exit(22, NULL);
		}
	}

	if (has_unit)
		*has_unit = unit;

	uint64_t val = strtoull(s, NULL, 10) << shift;
	return (int64_t)val;
}

static int64_t parse_size_kb(const char *s)
{
	int has_unit = 0;
	int64_t val = parse_num_with_unit(s, &has_unit);
	if (has_unit)
		val >>= 10;
	if (val <= 0) {
		fprintf(stderr, "Size must be greater than 0!\n");
		print_usage();
		mlc_exit(22, NULL);
	}
	return val;
}

static void check_birch_stream_platform(void)
{
	uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
	__asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
	uint32_t family = ((eax >> 8) & 0xf) + ((eax >> 20) & 0xff);
	uint32_t model = ((eax >> 4) & 0xf) | ((eax >> 12) & 0xf0);
	if (family == 6 && (model == 0xad || model == 0xae || model == 0xaf)) {
		printf("Detected Birch Stream platform, latency optimized mode may need to be turned on to get the best latencies. \n");
	}
}

static void check_hugepages(bool use_1gb, int num_nodes)
{
	if (use_1gb) {
		int free_1gb = read_proc_int("/sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages");
		if (free_1gb < 1) {
			fprintf(stderr,
				"\n*****   1GB Hugepages need to be allocated using /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages *****\n\n");
			mlc_exit(22, NULL);
		}
	} else {
		int nr_2mb = read_proc_int("/proc/sys/vm/nr_hugepages");
		if (nr_2mb >= 0 && nr_2mb < num_nodes * 1000) {
			fprintf(stderr, "*************   We need at least 1000 2M pages per numa node\n");
			fprintf(stderr, "*************   Adjust %d below to account for # of NUMA nodes\n", num_nodes * 2000);
			fprintf(stderr, "*************   and execute the following and re-run\n");
			fprintf(stderr, "*************   echo %d > /proc/sys/vm/nr_hugepages\n\n", num_nodes * 2000);
		}
	}
}

static bool is_valid_traffic_type(int w)
{
	return (w >= 2 && w <= 12) || w == 19 || w == 21 || w == 23 || w == 27;
}

static bool cpu_has_avx2(void)
{
	uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
	__asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
	if (eax < 7)
		return false;
	__asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
	if ((ecx & (1U << 27)) == 0)
		return false;
	uint32_t xcr0_lo = 0, xcr0_hi = 0;
	__asm__ volatile ("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
	if ((xcr0_lo & 0x6U) != 0x6U)
		return false;
	__asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
	return (ebx & (1U << 5)) != 0;
}

static bool cpu_has_avx512f(void)
{
	uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
	__asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
	if (eax < 7)
		return false;
	__asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
	if ((ecx & (1U << 27)) == 0)
		return false;
	uint32_t xcr0_lo = 0, xcr0_hi = 0;
	__asm__ volatile ("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
	if ((xcr0_lo & 0xe6U) != 0xe6U)
		return false;
	__asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
	return (ebx & (1U << 16)) != 0;
}

static int32_t parse_ddio_int_arg(const char *val_str)
{
	if (!val_str || *val_str == '\0') {
		printf("Parameter value %s is not number\n",
		       val_str ? val_str : "");
		mlc_exit(22, NULL);
	}
	for (const char *p = val_str; *p != '\0'; p++) {
		if (!isdigit((unsigned char)*p)) {
			printf("Parameter value %s is not number\n", val_str);
			mlc_exit(22, NULL);
		}
	}
	return (int32_t)strtol(val_str, NULL, 10);
}

static void print_cmdline_params(int argc, char *argv[])
{
	printf("Command line parameters: ");
	for (int i = 1; i < argc; i++) {
		printf("%s ", argv[i]);
	}
	printf("\n\n");
}

int main(int argc, char *argv[])
{
	setbuf(stdout, NULL);
	printf("Intel(R) Memory Latency Checker - %s\n", MLC_VERSION);

	if (argc == 2 && strcmp(argv[1], "-h") == 0) {
		print_usage();
		return 0;
	}

	setup_signal_handlers();

	bool count_only_online_cpus = false;
	bool print_cpuid = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--count_only_online_CPUs") == 0)
			count_only_online_cpus = true;
		else if (strcmp(argv[i], "--print_cpuid") == 0)
			print_cpuid = true;
	}

	helper_init_tsc();
	numa_init_topology_ex(count_only_online_cpus);
	int total_cpus = numa_get_total_cpus();
	int num_nodes = numa_get_num_nodes();
	msr_init(total_cpus);

	size_t buf_size_mb = DEFAULT_LATENCY_BUF_MB;
	double duration_sec = DEFAULT_TIME_SEC;
	bool random_access = false;
	bool explicit_r0 = false;
	bool verbose = false;
	bool keep_prefetchers = false;
	bool has_e = false;
	bool throughput_only = false;
	c2c_engine_t c2c_engine = C2C_ENGINE_READERBW;
	bool full_l3_matrix = false;
	bool has_c2cbw_subopt = false;
	int c2cbw_pref_mode = 0;	/* 1: --c2cbw_pref, 2: --c2cbw_nopref */
	const char *c2cbw_writer_str = NULL;
	const char *c2cbw_reader_str = NULL;
	const char *c2cbw_sharer_str = NULL;
	bool c2cbw_verbose = false;
	int c2cbw_delay = 0;
	bool c2cbw_stepped = false;
	bool c2cbw_partial = false;
	int c2cbw_ngroups = 1;
	int64_t c2cbw_skip_kb = -1;
	bool c2cbw_auto_sharer = false;
	bool all_cores_latency = false;
	bool single_thread_per_core = false;
	int target_core = -1;
	int writer_core = -1;
	int c2c_3rd_core = -1;
	const char *pstate_cores = NULL;
	int histogram_granularity = 10;
	int scan_node = 0;
	bool ddio_mode = false;
	const char *ddio_shm_name = DDIO_DEFAULT_SHM_NAME;
	int32_t ddio_session_id = 1;
	int32_t ddio_test_type = 0;
	int32_t ddio_slowmode_ms = 0;
	bool physaddr_dump = false;
	bool use_1gb_pages = false;
	bool legacy_mode = false;
	int stride = 64;
	int traffic_W = 2;
	bool has_W = false;
	bool has_R = false;
	bool has_m = false;
	bool has_k = false;
	bool has_o = false;
	const char *per_thread_file = NULL;
	bool has_Y = false;
	bool has_Z = false;
	int z_burst = 0;
	int single_delay = -1;
	bool has_j = false;
	bool has_n = false;
	bool has_U = false;
	bool has_x = false;
	int iters_x = 0;
	bool has_fastdelay = false;
	bool has_b = false;
	bool has_t = false;
	bool per_thread_bw = false;
	bool shared_buf = false;
	const char *cpu_list_str = NULL;
	const char *cpu_mask_str = NULL;
	size_t conc_hotbuf_kb = 4096;
	int conc_hot_pct = 75;
	bool has_d = false;
	bool has_D = false;
	int random_range_D = 4096;
	bool has_A = false;
	bool has_B = false;
	bool has_c = false;
	bool has_C = false;
	size_t c2c_window_bytes = 0;
	bool has_E = false;
	size_t c2c_writer_extra_bytes = 0;
	bool c2c_do_clflush = true;
	bool has_G = false;
	size_t c2c_reader_extra_bytes = 0;
	bool has_H = false;
	int init_core = 0;
	bool has_M = false;
	const char *pattern_file = NULL;
	bool has_N = false;
	bool has_S = false;
	bool has_w = false;
	bool has_f = false;
	bool has_g = false;
	bool has_i = false;
	bool has_I = false;
	bool has_l = false;
	bool has_L = false;
	bool has_p = false;
	bool has_P = false;
	bool has_q = false;
	bool has_r = false;
	bool has_s = false;
	bool has_Q = false;
	bool has_z = false;
	bool has_O = false;
	int64_t c2c_iters_total = 20000000LL;
	int64_t buf_size_kb = (int64_t)DEFAULT_BW_BUF_PER_THREAD_MB * 1024;
	bool has_dslots = false;
	delay_pc_config_t dpc_cfg = {
		.dslots = DELAY_PC_DEFAULT_SLOTS,
		.diters = DELAY_PC_DEFAULT_ITERS,
		.dstride = DELAY_PC_DEFAULT_STRIDE,
		.dsweep = DELAY_PC_DEFAULT_SWEEP,
		.dchain = DELAY_PC_DEFAULT_CHAIN,
		.dcore_mhz = DELAY_PC_DEFAULT_CORE_MHZ,
		.doverhead = DELAY_PC_DEFAULT_OVERHEAD,
		.target_core = -1
	};

	enum {
		MODE_DEFAULT,
		MODE_IDLE_LATENCY,
		MODE_LATENCY_MATRIX,
		MODE_BANDWIDTH_MATRIX,
		MODE_PEAK_BANDWIDTH,
		MODE_MAX_BANDWIDTH,
		MODE_LOADED_LATENCY,
		MODE_C2C_LATENCY,
		MODE_C2C_BANDWIDTH,
		MODE_CONCURRENT_BANDWIDTH,
		MODE_MEMORY_SCAN,
		MODE_DELAY_POINTER_CHASE,
		MODE_LATENCY_HISTOGRAM,
		MODE_HELP
	} mode = MODE_DEFAULT;

	int mode_count = 0;
#define SET_MODE(m) do { \
	mode_count++; \
	if (mode_count > 1) { \
		fprintf(stderr, "Only one test type mode with -- option can be specified\n"); \
		mlc_exit(22, NULL); \
	} \
	mode = (m); \
} while (0)

	for (int i = 1; i < argc; i++) {
		if (argv[i][0] != '-') {
			fprintf(stderr, "Unsupported argument %s in command line\n", argv[i]);
			mlc_exit(22, NULL);
		}

		if (argv[i][1] == '-') {
			/* Long options */
			const char *opt = argv[i] + 2;
			if (strcmp(opt, "idle_latency") == 0) {
				SET_MODE(MODE_IDLE_LATENCY);
			} else if (strcmp(opt, "latency_matrix") == 0) {
				SET_MODE(MODE_LATENCY_MATRIX);
			} else if (strcmp(opt, "bandwidth_matrix") == 0) {
				SET_MODE(MODE_BANDWIDTH_MATRIX);
			} else if (strcmp(opt, "peak_injection_bandwidth") == 0) {
				SET_MODE(MODE_PEAK_BANDWIDTH);
			} else if (strcmp(opt, "max_bandwidth") == 0) {
				SET_MODE(MODE_MAX_BANDWIDTH);
			} else if (strcmp(opt, "peak_bandwidth") == 0) {
				fprintf(stderr, "peak_bandwidth name has been changed. Use peak_injection_bandwidth instead\n");
				mlc_exit(22, NULL);
			} else if (strcmp(opt, "loaded_latency") == 0) {
				SET_MODE(MODE_LOADED_LATENCY);
			} else if (strcmp(opt, "c2c_latency") == 0) {
				SET_MODE(MODE_C2C_LATENCY);
			} else if (strcmp(opt, "c2c_bandwidth_matrix") == 0 ||
				   strcmp(opt, "c2c_readerbw_matrix") == 0 ||
				   strcmp(opt, "c2c_reader_bw_matrix") == 0) {
				SET_MODE(MODE_C2C_BANDWIDTH);
				c2c_engine = C2C_ENGINE_MATRIX;
			} else if (strcmp(opt, "c2c_bandwidth_pipelined") == 0) {
				SET_MODE(MODE_C2C_BANDWIDTH);
				c2c_engine = C2C_ENGINE_PIPELINED;
			} else if (strcmp(opt, "c2c_bandwidth") == 0 ||
				   strcmp(opt, "c2c_readerbw") == 0 ||
				   strcmp(opt, "c2c_reader_bw") == 0) {
				SET_MODE(MODE_C2C_BANDWIDTH);
				c2c_engine = C2C_ENGINE_READERBW;
			} else if (strncmp(opt, "c2cbw_reader", 12) == 0 &&
				   (opt[12] == '=' || opt[12] != '\0')) {
				const char *vp = opt + 12;
				if (*vp == '=')
					vp++;
				c2cbw_reader_str = vp;
				has_c2cbw_subopt = true;
			} else if (strncmp(opt, "c2cbw_writer", 12) == 0 &&
				   (opt[12] == '=' || opt[12] != '\0')) {
				const char *vp = opt + 12;
				if (*vp == '=')
					vp++;
				c2cbw_writer_str = vp;
				has_c2cbw_subopt = true;
			} else if (strncmp(opt, "c2cbw_sharer", 12) == 0 &&
				   (opt[12] == '=' || opt[12] != '\0')) {
				const char *vp = opt + 12;
				if (*vp == '=')
					vp++;
				c2cbw_sharer_str = vp;
				has_c2cbw_subopt = true;
			} else if (strcmp(opt, "c2cbw_verbose") == 0) {
				c2cbw_verbose = true;
				has_c2cbw_subopt = true;
			} else if (strncmp(opt, "c2cbw_delay", 11) == 0 &&
				   (opt[11] == '=' || isdigit((unsigned char)opt[11]))) {
				const char *vp = (opt[11] == '=') ? (opt + 12) : (opt + 11);
				c2cbw_delay = (int)parse_num_with_unit(vp, NULL);
				has_c2cbw_subopt = true;
			} else if (strcmp(opt, "c2cbw_stepped") == 0 ||
				   strcmp(opt, "c2cbw_steppedmode") == 0) {
				c2cbw_stepped = true;
				has_c2cbw_subopt = true;
			} else if (strcmp(opt, "c2cbw_pref") == 0) {
				c2cbw_pref_mode = 1;
				has_c2cbw_subopt = true;
			} else if (strcmp(opt, "c2cbw_nopref") == 0) {
				c2cbw_pref_mode = 2;
				has_c2cbw_subopt = true;
			} else if (strcmp(opt, "c2cbw_partial") == 0) {
				c2cbw_partial = true;
				has_c2cbw_subopt = true;
			} else if (strncmp(opt, "c2cbw_ngroups", 13) == 0 &&
				   (opt[13] == '=' || opt[13] != '\0')) {
				const char *vp = (opt[13] == '=') ? (opt + 14) : (opt + 13);
				char *endp = NULL;
				long ng = (*vp != '\0') ? strtol(vp, &endp, 10) : 0;
				if (!endp || *endp != '\0' || ng < 1) {
					fprintf(stderr, "--c2cbw_ngroups must be >= 1\n");
					mlc_exit(22, NULL);
				}
				c2cbw_ngroups = (int)ng;
				has_c2cbw_subopt = true;
			} else if (strncmp(opt, "c2cbw_skip", 10) == 0 &&
				   (opt[10] == '=' || opt[10] != '\0')) {
				const char *vp = (opt[10] == '=') ? (opt + 11) : (opt + 10);
				if (*vp == '-' || *vp == '\0') {
					fprintf(stderr, "--c2cbw_skip must be >= 0\n");
					mlc_exit(22, NULL);
				}
				int has_unit = 0;
				int64_t sk = parse_num_with_unit(vp, &has_unit);
				if (has_unit)
					sk >>= 10;
				if (sk < 0) {
					fprintf(stderr, "--c2cbw_skip must be >= 0\n");
					mlc_exit(22, NULL);
				}
				c2cbw_skip_kb = sk;
				has_c2cbw_subopt = true;
			} else if (strcmp(opt, "c2cbw_auto_sharer") == 0) {
				c2cbw_auto_sharer = true;
				has_c2cbw_subopt = true;
			} else if (strcmp(opt, "concurrent_bandwidth") == 0) {
				SET_MODE(MODE_CONCURRENT_BANDWIDTH);
			} else if (strcmp(opt, "memory_scan") == 0 ||
				   strcmp(opt, "memory_bandwidth_scan") == 0) {
				SET_MODE(MODE_MEMORY_SCAN);
			} else if (strcmp(opt, "delay_pointer_chase") == 0) {
				SET_MODE(MODE_DELAY_POINTER_CHASE);
			} else if (strcmp(opt, "latency_histogram") == 0) {
				SET_MODE(MODE_LATENCY_HISTOGRAM);
			} else if (strcmp(opt, "help") == 0) {
				SET_MODE(MODE_HELP);
			} else if (strcmp(opt, "legacy_mode") == 0) {
				legacy_mode = true;
			} else if (strcmp(opt, "full_l3_matrix") == 0) {
				full_l3_matrix = true;
			} else if (strcmp(opt, "per_llc_cluster") == 0) {
				/* run per-L3-cluster measurements */
			} else if (strncmp(opt, "scan_node=", 10) == 0) {
				scan_node = atoi(opt + 10);
			} else if (strncmp(opt, "ddiomode", 8) == 0) {
				ddio_mode = true;
				if (verbose)
					puts("DDIO mode detected. ");
			} else if (strncmp(opt, "ddiomapname", 11) == 0) {
				ddio_shm_name = argv[i] + 13;
				if (*ddio_shm_name == '\0') {
					fprintf(stderr,
						"Error: --ddiomapname must be followed by a name.\n");
					return 1;
				}
				if (verbose)
					printf("DDIO shared memory map name: %s \n",
					       ddio_shm_name);
			} else if (strncmp(opt, "ddio_sessionID", 14) == 0) {
				ddio_session_id =
				    parse_ddio_int_arg(argv[i] + 16);
				if (verbose)
					printf("DDIO session ID is:%d\n",
					       ddio_session_id);
			} else if (strncmp(opt, "ddio_testtype", 13) == 0) {
				ddio_test_type =
				    parse_ddio_int_arg(argv[i] + 15);
				if (verbose)
					printf("DDIO test type is:%d\n",
					       ddio_test_type);
			} else if (strncmp(opt, "ddio_slowmode", 13) == 0) {
				ddio_slowmode_ms =
				    parse_ddio_int_arg(argv[i] + 15);
				if (verbose)
					printf("DDIO slow mode delay is:%d milliseconds \n",
					       ddio_slowmode_ms);
			} else if (strcmp(opt, "physaddr_dump") == 0) {
				physaddr_dump = true;
				if (verbose)
					puts("Physical address dumps are enabled. It is only supported for loaded_latency use cases. ");
			} else if (strncmp(opt, "fastdelay", 9) == 0) {
				has_fastdelay = true;
			} else if (strncmp(opt, "lat_hist_max_us", 15) == 0) {
				/* lat_hist_max_us */
			} else if (strncmp(opt, "lat_hist_max_cycles", 19) == 0) {
				/* lat_hist_max_cycles */
			} else if (strncmp(opt, "conc_hotbuf", 11) == 0) {
				const char *val_p = opt + 11;
				if (*val_p == '=')
					val_p++;
				if (*val_p == '\0') {
					print_usage();
					mlc_exit(22, "--conc_hotbuf requires a positive size in KB (e.g. --conc_hotbuf4096)\n");
				}
				int has_unit = 0;
				int64_t kb = parse_num_with_unit(val_p, &has_unit);
				if (has_unit)
					kb >>= 10;
				if (kb < 1) {
					print_usage();
					mlc_exit(22, "--conc_hotbuf requires a positive size in KB (e.g. --conc_hotbuf4096)\n");
				}
				conc_hotbuf_kb = (size_t)kb;
			} else if (strncmp(opt, "conc_hot_pct", 12) == 0) {
				const char *val_p = opt + 12;
				if (*val_p == '=')
					val_p++;
				char *endp = NULL;
				long pct = (*val_p != '\0') ? strtol(val_p, &endp, 10) : -1;
				int valid_pcts[] = {0, 10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 87, 90, 93, 100};
				bool ok = (endp && *endp == '\0' && *val_p != '\0');
				if (ok) {
					ok = false;
					for (size_t p = 0; p < sizeof(valid_pcts) / sizeof(valid_pcts[0]); p++) {
						if (pct == valid_pcts[p]) {
							ok = true;
							break;
						}
					}
				}
				if (!ok) {
					print_usage();
					mlc_exit(22, "--conc_hot_pct must be 0, 10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 87, 90, 93, or 100\n");
				}
				conc_hot_pct = (int)pct;
			} else if (strncmp(opt, "dchain", 6) == 0) {
				dpc_cfg.dchain = (int)parse_num_with_unit(opt + 6, NULL);
				if (verbose)
					printf("Delay pointer chase chain_len: %d\n", dpc_cfg.dchain);
			} else if (strncmp(opt, "dstride", 7) == 0) {
				dpc_cfg.dstride = (int)parse_num_with_unit(opt + 7, NULL);
				if (verbose)
					printf("Delay pointer chase stride: %d\n", dpc_cfg.dstride);
			} else if (strncmp(opt, "dsweep", 6) == 0) {
				dpc_cfg.dsweep = (int)parse_num_with_unit(opt + 6, NULL);
				if (verbose)
					printf("Delay pointer chase sweep max: %d\n", dpc_cfg.dsweep);
			} else if (strncmp(opt, "dslots", 6) == 0) {
				int64_t v = parse_num_with_unit(opt + 6, NULL);
				if (v <= 0 || v > INT32_MAX) {
					fprintf(stderr, "mlc: --dslots value must be greater than 0\n");
					mlc_exit(22, NULL);
				}
				has_dslots = true;
				dpc_cfg.dslots = (size_t)v;
				if (verbose)
					printf("Delay pointer chase nslots: %zu\n", dpc_cfg.dslots);
			} else if (strncmp(opt, "diters", 6) == 0) {
				dpc_cfg.diters = (int)parse_num_with_unit(opt + 6, NULL);
				if (verbose)
					printf("Delay pointer chase iters: %d\n", dpc_cfg.diters);
			} else if (strncmp(opt, "dcore_mhz", 9) == 0) {
				dpc_cfg.dcore_mhz = (int)parse_num_with_unit(opt + 9, NULL);
				if (verbose)
					printf("Delay pointer chase core MHz: %d\n", dpc_cfg.dcore_mhz);
			} else if (strncmp(opt, "doverhead", 9) == 0) {
				dpc_cfg.doverhead = (int)parse_num_with_unit(opt + 9, NULL);
				if (verbose)
					printf("Delay pointer chase overhead: %d\n", dpc_cfg.doverhead);
			} else if (strcmp(opt, "count_only_online_CPUs") == 0 ||
				   strcmp(opt, "print_cpuid") == 0 ||
				   strncmp(opt, "bwscan_", 7) == 0 ||
				   strncmp(opt, "delayunroll", 11) == 0) {
				/* supported secondary options */
			} else {
				char err_buf[256];
				snprintf(err_buf, sizeof(err_buf), "Invalid Argument: %s\n", argv[i]);
				print_usage();
				mlc_exit(22, err_buf);
			}
		} else {
			/* Short options */
			char opt = argv[i][1];
			const char *val_str = argv[i] + 2;

			switch (opt) {
			case '?':
				print_usage();
				return 0;
			case 'a':
				check_arg_no_val('a', val_str);
				all_cores_latency = true;
				break;
			case 'A':
				check_arg_has_val('A', val_str);
				has_A = true;
				break;
			case 'B':
				check_arg_no_val('B', val_str);
				per_thread_bw = true;
				has_B = true;
				break;
			case 'b':
				check_arg_has_val('b', val_str);
				has_b = true;
				buf_size_kb = parse_size_kb(val_str);
				if (buf_size_kb >= 1024)
					buf_size_mb = (size_t)(buf_size_kb / 1024);
				else if (buf_size_kb > 0)
					buf_size_mb = 1;
				break;
			case 'c':
				check_arg_has_val('c', val_str);
				has_c = true;
				target_core = atoi(val_str);
				if (target_core < 0 || target_core >= total_cpus) {
					fprintf(stderr, "Minimum value for core number is 0 and maximum value is %d\nCore number must be in this range! \n", total_cpus - 1);
					mlc_exit(22, NULL);
				}
				break;
			case 'C':
				check_arg_has_val('C', val_str);
				has_C = true;
				c2c_window_bytes =
				    (size_t)parse_size_kb(val_str) << 10;
				break;
			case 'd':
				check_arg_has_val('d', val_str);
				has_d = true;
				single_delay = atoi(val_str);
				if (single_delay < 0) {
					fprintf(stderr, "Delay value must be non-negative!\n");
					mlc_exit(22, NULL);
				}
				break;
			case 'D':
				check_arg_has_val('D', val_str);
				has_D = true;
				random_range_D =
				    (int)parse_num_with_unit(val_str, NULL);
				if (random_range_D < 2) {
					printf("Random access range must be at least 2 cache lines; %d was given\n",
					       random_range_D);
					mlc_exit(22, NULL);
				}
				break;
			case 'e':
				check_arg_no_val('e', val_str);
				keep_prefetchers = true;
				has_e = true;
				break;
			case 'E':
				check_arg_has_val('E', val_str);
				has_E = true;
				{
					int64_t kb =
					    parse_num_with_unit(val_str, NULL);
					if (kb < 0 ||
					    kb * 1024LL > 0x7fffffffLL) {
						printf("-E: maximum value is %d KB\n",
						       0x1fffff);
						puts("The extra buffer size is held in an int, and a larger value cannot be stored without being truncated.");
						mlc_exit(22, NULL);
					}
					c2c_writer_extra_bytes =
					    (size_t)(kb * 1024LL);
				}
				break;
			case 'F':
				check_arg_no_val('F', val_str);
				c2c_do_clflush = false;
				break;
			case 'f':
				check_arg_has_val('f', val_str);
				has_f = true;
				{
					int exp = atoi(val_str);
					if (exp >= 0 && exp <= 30)
						histogram_granularity = 1 << exp;
				}
				break;
			case 'g':
				check_arg_has_val('g', val_str);
				has_g = true;
				break;
			case 'G':
				check_arg_has_val('G', val_str);
				has_G = true;
				{
					int64_t kb =
					    parse_num_with_unit(val_str, NULL);
					if (kb < 0 ||
					    kb * 1024LL > 0x7fffffffLL) {
						printf("-G: maximum value is %d KB\n",
						       0x1fffff);
						puts("The extra buffer size is held in an int, and a larger value cannot be stored without being truncated.");
						mlc_exit(22, NULL);
					}
					c2c_reader_extra_bytes =
					    (size_t)(kb * 1024LL);
				}
				break;
			case 'h':
				check_arg_no_val('h', val_str);
				use_1gb_pages = true;
				numa_set_hugepage_mode(HUGEPAGE_1GB);
				break;
			case 'H':
				check_arg_no_val('H', val_str);
				has_H = true;
				break;
			case 'i':
				check_arg_has_val('i', val_str);
				has_i = true;
				init_core =
				    (int)parse_num_with_unit(val_str, NULL);
				if (init_core < 0 || init_core >= total_cpus) {
					printf("Minimum value for i is 0 and maximum value is %d\n",
					       total_cpus - 1);
					puts("Number must be in this range! \n");
					mlc_exit(22, NULL);
				}
				break;
			case 'I':
				check_arg_no_val('I', val_str);
				has_I = true;
				break;
			case 'j':
				check_arg_has_val('j', val_str);
				has_j = true;
				scan_node = atoi(val_str);
				break;
			case 'J':
				check_arg_has_val('J', val_str);
				break;
			case 'k':
				check_arg_has_val('k', val_str);
				has_k = true;
				cpu_list_str = val_str;
				break;
			case 'K':
				check_arg_has_val('K', val_str);
				break;
			case 'l':
				check_arg_has_val('l', val_str);
				has_l = true;
				stride = atoi(val_str);
				if (stride <= 0) {
					fprintf(stderr, "Stride length must be greater than zero. -l is the distance in bytes between consecutive accesses, and the default is 64. Exiting..\n");
					mlc_exit(22, NULL);
				}
				break;
			case 'L':
				check_arg_no_val('L', val_str);
				has_L = true;
				numa_set_hugepage_mode(HUGEPAGE_2MB);
				break;
			case 'm':
				check_arg_has_val('m', val_str);
				has_m = true;
				cpu_mask_str = val_str;
				break;
			case 'M':
				check_arg_has_val('M', val_str);
				has_M = true;
				pattern_file = val_str;
				break;
			case 'N':
				check_arg_has_val('N', val_str);
				has_N = true;
				break;
			case 'n':
				check_arg_has_val('n', val_str);
				has_n = true;
				scan_node = atoi(val_str);
				break;
			case 'o':
				check_arg_has_val('o', val_str);
				has_o = true;
				per_thread_file = val_str;
				break;
			case 'O':
				check_arg_no_val('O', val_str);
				has_O = true;
				break;
			case 'p':
				check_arg_has_val('p', val_str);
				has_p = true;
				pstate_cores = val_str;
				break;
			case 'P':
				check_arg_no_val('P', val_str);
				has_P = true;
				break;
			case 'q':
				check_arg_has_val('q', val_str);
				has_q = true;
				break;
			case 'Q':
				check_arg_no_val('Q', val_str);
				has_Q = true;
				break;
			case 'r':
				has_r = true;
				if (val_str && strcmp(val_str, "0") == 0) {
					explicit_r0 = true;
					random_access = false;
				} else {
					check_arg_no_val('r', val_str);
					random_access = true;
				}
				break;
			case 'R':
				check_arg_no_val('R', val_str);
				has_R = true;
				break;
			case 's':
				check_arg_has_val('s', val_str);
				has_s = true;
				break;
			case 'S':
				check_arg_has_val('S', val_str);
				has_S = true;
				c2c_3rd_core = atoi(val_str);
				if (c2c_3rd_core < 0 || c2c_3rd_core >= total_cpus) {
					fprintf(stderr, "-S: minimum value for core number is 0 and maximum value is %d\nCore number must be in this range! \n", total_cpus - 1);
					mlc_exit(22, NULL);
				}
				break;
			case 't':
				check_arg_has_val('t', val_str);
				has_t = true;
				duration_sec = atoi(val_str);
				if (duration_sec <= 0) {
					fprintf(stderr, "Time to run must be greater than zero!\n");
					mlc_exit(22, NULL);
				}
				break;
			case 'T':
				check_arg_no_val('T', val_str);
				throughput_only = true;
				break;
			case 'u':
				check_arg_no_val('u', val_str);
				shared_buf = true;
				break;
			case 'U':
				check_arg_no_val('U', val_str);
				has_U = true;
				break;
			case 'v':
				check_arg_no_val('v', val_str);
				verbose = true;
				break;
			case 'w':
				check_arg_has_val('w', val_str);
				has_w = true;
				writer_core = atoi(val_str);
				if (writer_core < 0 || writer_core >= total_cpus) {
					fprintf(stderr, "-w: minimum value for core number is 0 and maximum value is %d\nCore number must be in this range! \n", total_cpus - 1);
					mlc_exit(22, NULL);
				}
				break;
			case 'W':
				check_arg_has_val('W', val_str);
				has_W = true;
				traffic_W = atoi(val_str);
				if (!is_valid_traffic_type(traffic_W)) {
					fprintf(stderr, "Unsupported -W option!\n");
					mlc_exit(22, NULL);
				}
				break;
			case 'x':
				check_arg_has_val('x', val_str);
				has_x = true;
				{
					int64_t xv =
					    parse_num_with_unit(val_str, NULL);
					iters_x = (int)xv;
					c2c_iters_total = xv * 1000000LL;
					if (c2c_iters_total < 0) {
						fprintf(stderr, "Number of iterations should be positive!\n");
						mlc_exit(22, NULL);
					}
				}
				break;
			case 'X':
				check_arg_no_val('X', val_str);
				single_thread_per_core = true;
				break;
			case 'Y':
				check_arg_no_val('Y', val_str);
				if (!cpu_has_avx2()) {
					fprintf(stderr, "AVX instructions not supported by this processor\n");
					mlc_exit(22, NULL);
				}
				has_Y = true;
				break;
			case 'Z':
				if (val_str && *val_str != '\0') {
					if (strcmp(val_str, "1") != 0 &&
					    strcmp(val_str, "2") != 0 &&
					    strcmp(val_str, "4") != 0) {
						fprintf(stderr,
							"Burst size given to -Z must be 1, 2 or 4. Got '%s'.\n",
							val_str);
						mlc_exit(22, NULL);
					}
					z_burst = atoi(val_str);
				}
				if (!cpu_has_avx512f()) {
					fprintf(stderr, "AVX-512 instructions not supported by this processor\n");
					mlc_exit(22, NULL);
				}
				has_Z = true;
				break;
			case 'z':
				check_arg_no_val('z', val_str);
				has_z = true;
				break;
			default:
				{
					char err_buf[256];
					snprintf(err_buf, sizeof(err_buf), "Invalid Argument: %s\n", argv[i]);
					print_usage();
					mlc_exit(22, err_buf);
				}
				break;
			}
		}
	}

	if (has_W && has_R) {
		fprintf(stderr, "read-only load generated, if this is used, -W option should NOT be used\n");
		mlc_exit(22, NULL);
	}
	if (has_W && (traffic_W == 21 || traffic_W == 23 || traffic_W == 27) && !has_o) {
		fprintf(stderr,
			"Traffic type -W%d mixes two address sources, and the ratio between\n"
			"them can only be given as the address mix ratio field of an -o\n"
			"configuration file. Supply one, or choose a single source traffic type.\n",
			traffic_W);
		mlc_exit(22, NULL);
	}
	if (has_W && (traffic_W == 21 || traffic_W == 23 || traffic_W == 27) && !cpu_has_avx2()) {
		fprintf(stderr, "W%d requires a CPU with AVX2 support\n", traffic_W);
		mlc_exit(22, NULL);
	}
	if (has_W && traffic_W == 9 && (has_Y || has_Z)) {
		fprintf(stderr, "WARNING: W9 (3R/1NTW) has no AVX2/AVX512 kernel; using SSE instead\n");
	}
	if (has_Z && z_burst > 0 && has_W) {
		printf("NOTE: -Z%d selects a burst size, which only applies to 100%% read traffic.\n"
		       "      This run uses -W%d, so the burst size is ignored.\n",
		       z_burst, traffic_W);
	}
	if (has_m && has_k) {
		fprintf(stderr, "Only either -m or -k option can be specified.  Exiting...\n");
		mlc_exit(22, NULL);
	}
	if (single_thread_per_core) {
		if (mode == MODE_CONCURRENT_BANDWIDTH) {
			if (has_m || has_o) {
				fprintf(stderr, "-X can't be specified if -m or -o is also specified! Exiting...\n");
				mlc_exit(22, NULL);
			}
		} else if (has_m || has_o || has_k) {
			fprintf(stderr, "-X can't be specified if -m or -o or -k is also specified! Exiting...\n");
			mlc_exit(22, NULL);
		}
	}
	if (has_m && has_j) {
		fprintf(stderr, "Only either -m or -j option can be specified.  Exiting...\n");
		mlc_exit(22, NULL);
	}
	if (has_o && has_m) {
		fprintf(stderr, "Only either -o or -m option can be specified.  Exiting...\n");
		mlc_exit(22, NULL);
	}
	if (has_o && has_k) {
		fprintf(stderr, "Only either -o or -k option can be specified.  Exiting...\n");
		mlc_exit(22, NULL);
	}
	if (has_n && !has_U && mode != MODE_MEMORY_SCAN &&
	    mode != MODE_CONCURRENT_BANDWIDTH) {
		fprintf(stderr, "-U option needs to be specified if -n is present.  Exiting...\n");
		mlc_exit(22, NULL);
	}
	if (has_fastdelay && mode != MODE_LOADED_LATENCY) {
		fprintf(stderr, "--fastdelay/--fastdelayfile are only valid with --loaded_latency.  Exiting...\n");
		mlc_exit(22, NULL);
	}
	if (mode == MODE_LOADED_LATENCY && has_x && iters_x != 0) {
		fprintf(stderr, "Loaded latency accepts only -x0, which measures a single iteration. Use -t to set the run time in seconds instead. Exiting..\n");
		mlc_exit(22, NULL);
	}
	if (mode == MODE_DELAY_POINTER_CHASE && has_b && !has_dslots) {
		if (buf_size_kb > INT32_MAX) {
			fprintf(stderr,
				"mlc: -b value %d KiB is too large for --delay_pointer_chase (slot count would overflow)\n",
				(int)buf_size_kb);
			mlc_exit(22, NULL);
		}
		dpc_cfg.dslots = (size_t)buf_size_kb << 4;
	}

	if (mode == MODE_HELP) {
		print_usage();
		return 0;
	}

	if (mode == MODE_CONCURRENT_BANDWIDTH) {
		const struct {
			bool set;
			const char *name;
		} unsup[] = {
			{ all_cores_latency, "-a" },
			{ has_d, "-d" },
			{ has_D, "-D" },
			{ has_f, "-f" },
			{ has_g, "-g" },
			{ has_G, "-G" },
			{ has_i, "-i" },
			{ has_I, "-I" },
			{ has_l, "-l" },
			{ has_n, "-n" },
			{ has_N, "-N" },
			{ has_o, "-o" },
			{ has_p, "-p" },
			{ has_P, "-P" },
			{ has_q, "-q" },
			{ has_R, "-R" },
			{ has_r, "-r" },
			{ has_s, "-s" },
			{ has_Q, "-Q" },
			{ throughput_only, "-T" },
			{ has_W, "-W" },
			{ has_Y, "-Y" },
			{ has_Z, "-Z" },
			{ has_z, "-z" },
			{ has_O, "-O" },
			{ has_x, "-x" }
		};
		char flag_list[256] = { 0 };
		int unsup_cnt = 0;
		for (size_t u = 0; u < sizeof(unsup) / sizeof(unsup[0]); u++) {
			if (!unsup[u].set)
				continue;
			if (unsup_cnt > 0) {
				strncat(flag_list, ", ",
					sizeof(flag_list) - strlen(flag_list) - 1);
			}
			strncat(flag_list, unsup[u].name,
				sizeof(flag_list) - strlen(flag_list) - 1);
			unsup_cnt++;
		}
		if (unsup_cnt > 0) {
			char err_msg[512];
			snprintf(err_msg, sizeof(err_msg),
				 "Unsupported flag%s for --concurrent_bandwidth: %s\n",
				 (unsup_cnt == 1) ? "" : "s", flag_list);
			print_usage();
			mlc_exit(22, err_msg);
		}
	}

	if (mode == MODE_C2C_LATENCY) {
		if (all_cores_latency || has_A || has_d || has_f || has_g ||
		    has_I || has_j || has_k || has_m || has_M || has_n ||
		    has_N || has_o || has_p || has_P || has_Q || has_q ||
		    has_R || has_s || has_t || throughput_only || shared_buf ||
		    has_U || has_W || has_O || single_thread_per_core ||
		    has_Y || has_Z || has_z) {
			printf("Command line parameters: ");
			for (int i = 1; i < argc; i++)
				printf("%s ", argv[i]);
			printf("\n");
			fprintf(stderr,
				"Unsupported Argument for c2c_latency! \n");
			print_usage();
			mlc_exit(22, NULL);
		}
	}

	if (mode == MODE_PEAK_BANDWIDTH || mode == MODE_MAX_BANDWIDTH) {
		if (all_cores_latency || has_B || has_d || has_D || has_f ||
		    has_g || has_i || has_I || has_n || has_N || has_o ||
		    has_p || has_P || has_q || has_R || has_r || has_s ||
		    has_Q || throughput_only || has_W || has_z || has_O ||
		    has_j || has_x) {
			print_cmdline_params(argc, argv);
			double err_mib =
			    has_b ? ((double)buf_size_kb / 1024.0)
				  : (double)DEFAULT_BW_BUF_PER_THREAD_MB;
			printf("Using buffer size of %.3fMiB/thread for reads and an additional %.3fMiB/thread for writes\n",
			       err_mib, err_mib);
			fprintf(stderr,
				"Unsupported Argument for peak_bandwidth! \n\n");
			print_usage();
			mlc_exit(22, NULL);
		}
	}

	if (has_c2cbw_subopt && mode == MODE_DEFAULT) {
		mode = MODE_C2C_BANDWIDTH;
	}

	if (has_j && numa_is_numa_mode() && !numa_get_node_by_id(scan_node)) {
		fprintf(stderr, "Invalid NUMA node %d specified with -j\n",
			scan_node);
		mlc_exit(22, NULL);
	}

	if (print_cpuid || verbose) {
		numa_print_cpuid_topology(verbose);
	}

	check_birch_stream_platform();
	if (mode == MODE_C2C_LATENCY && cpu_is_amd_processor() &&
	    !has_L && !use_1gb_pages) {
		has_L = true;
		numa_set_hugepage_mode(HUGEPAGE_2MB);
	}
	if (cpu_is_amd_processor() || has_L || use_1gb_pages) {
		check_hugepages(use_1gb_pages, num_nodes);
	}

	if (mode != MODE_MEMORY_SCAN) {
		manage_numa_balancing();

		if (msr_is_amd()) {
			if (!msr_is_amd_zen3_plus()) {
				if (verbose && !keep_prefetchers) {
					puts("HW prefetcher cannot be modified as cpu is older than zen3");
				}
				keep_prefetchers = true;
			}
			random_access = !explicit_r0;
		}

		if (!keep_prefetchers && !msr_probe_modifiable(verbose)) {
			puts("*** Unable to modify prefetchers (try executing 'modprobe msr')");
			if (explicit_r0) {
				puts("*** -r0 given, so leaving the access pattern sequential as asked. The");
				puts("*** reported latency will be a prefetched cache hit, not a memory latency.");
			} else {
				puts("*** So, enabling random access for latency measurements");
			}
			random_access = !explicit_r0;
			keep_prefetchers = true;
		}

		if (has_e && !random_access) {
			puts("*** -e leaves the hardware prefetchers as they are, and this run uses a");
			puts("*** sequential access pattern. If the prefetchers are enabled, the reported");
			puts("*** latency will be a prefetched cache hit rather than a memory latency.");
			puts("*** Add -r to use a random access pattern if that is not what you want.");
		}
	}

	if (pstate_cores) {
		if (create_pstate_threads_on_other_cores(
			pstate_cores, target_core, verbose) != 0) {
			mlc_exit(22, NULL);
		}
	}

	if (verbose) {
		printf
		    ("Detected %d logical CPUs across %d socket(s) and %d NUMA node(s).\n",
		     total_cpus, numa_get_num_sockets(), num_nodes);
		printf("Calibrated TSC frequency: %.2f GHz\n",
		       helper_get_tsc_freq_ghz());
		printf("Hardware MSR interface: %s\n\n",
		       has_e ? "Unmodified (-e)" :
		       (msr_is_available() ? "Available" : "Not accessible"));
	}

	if (all_cores_latency) {
		run_idle_latency_all_cores(buf_size_mb, random_access,
					   single_thread_per_core,
					   keep_prefetchers, verbose);
		mlc_exit(0, NULL);
	}

	size_t bw_buf_mb = has_b ? buf_size_mb : DEFAULT_BW_BUF_PER_THREAD_MB;
	bw_set_simd_options(
	    has_Z ? BW_SIMD_AVX512 : (has_Y ? BW_SIMD_AVX2 : BW_SIMD_SSE2),
	    z_burst);
	bw_set_stride(stride);
	bw_set_single_delay(single_delay);
	bw_set_physaddr_dump(physaddr_dump && mode == MODE_LOADED_LATENCY);
	ddio_configure(ddio_mode, ddio_shm_name, ddio_session_id,
		       ddio_test_type, ddio_slowmode_ms, verbose);
	if (has_o && per_thread_file) {
		if (bw_load_per_thread_config(per_thread_file, total_cpus,
					      num_nodes) != 0) {
			mlc_exit(22, NULL);
		}
	} else {
		bw_clear_per_thread_config();
	}
	traffic_type_t eff_traffic =
	    has_W ? (traffic_type_t)traffic_W : TRAFFIC_ALL_READS;

	switch (mode) {
	case MODE_HELP:
		print_usage();
		break;
	case MODE_IDLE_LATENCY: {
		if (!has_b && buf_size_mb == DEFAULT_LATENCY_BUF_MB) {
			buf_size_mb = 2000;
		}
		int idle_alloc_node = has_j ? scan_node : -1;
		if (target_core >= 0) {
			run_idle_latency_core(target_core, buf_size_mb,
					      random_access, keep_prefetchers,
					      idle_alloc_node, verbose);
		} else {
			run_idle_latency(buf_size_mb, random_access,
					 keep_prefetchers, idle_alloc_node,
					 verbose);
		}
		break;
	}
	case MODE_LATENCY_MATRIX:
		print_cmdline_params(argc, argv);
		printf("Using buffer size of %.3fMiB\n", (double)buf_size_mb);
		run_latency_matrix(buf_size_mb, random_access,
				   keep_prefetchers, verbose);
		break;
	case MODE_BANDWIDTH_MATRIX:
		print_cmdline_params(argc, argv);
		if (!has_o) {
			printf
			    ("Using buffer size of %.3fMiB/thread for reads and an additional %.3fMiB/thread for writes\n",
			     (double)bw_buf_mb, (double)bw_buf_mb);
		}
		if (!keep_prefetchers)
			msr_enable_prefetchers(verbose);
		run_bandwidth_matrix(bw_buf_mb, duration_sec,
				     single_thread_per_core, verbose,
				     eff_traffic);
		break;
	case MODE_PEAK_BANDWIDTH:
	case MODE_MAX_BANDWIDTH: {
		print_cmdline_params(argc, argv);
		size_t peak_kb =
		    has_b ? (size_t)buf_size_kb
			  : ((size_t)DEFAULT_BW_BUF_PER_THREAD_MB * 1024ULL);
		double peak_mib = (double)peak_kb / 1024.0;
		printf
		    ("Using buffer size of %.3fMiB/thread for reads and an additional %.3fMiB/thread for writes\n\n",
		     peak_mib, peak_mib);
		if (!keep_prefetchers)
			msr_enable_prefetchers(verbose);
		peak_bw_options_t peak_opts = {
			.per_thread_kb = peak_kb,
			.duration_sec = duration_sec,
			.single_thread_per_core = single_thread_per_core,
			.max_bandwidth_mode = (mode == MODE_MAX_BANDWIDTH),
			.verbose = verbose,
			.cpu_list = has_k ? cpu_list_str : NULL,
			.cpu_mask = has_m ? cpu_mask_str : NULL,
			.pattern_file = has_M ? pattern_file : NULL
		};
		int rc = run_peak_or_max_bandwidth(&peak_opts);
		if (rc != 0)
			mlc_exit(rc, NULL);
		break;
	}
	case MODE_LOADED_LATENCY: {
		print_cmdline_params(argc, argv);
		size_t ll_kb =
		    has_b ? (size_t)buf_size_kb
			  : ((size_t)DEFAULT_BW_BUF_PER_THREAD_MB * 1024ULL);
		if (!has_o) {
			double ll_mib = (double)ll_kb / 1024.0;
			printf
			    ("Using buffer size of %.3fMiB/thread for reads and an additional %.3fMiB/thread for writes\n\n",
			     ll_mib, ll_mib);
		}
		loaded_lat_options_t ll_opts = {
			.per_thread_kb = ll_kb,
			.duration_sec = duration_sec,
			.random_latency = random_access,
			.keep_prefetchers = keep_prefetchers,
			.target_core = target_core,
			.single_thread_per_core = single_thread_per_core,
			.throughput_only = throughput_only,
			.verbose = verbose,
			.has_alloc_node = has_j,
			.alloc_node = scan_node,
			.traffic = eff_traffic,
			.cpu_list = has_k ? cpu_list_str : NULL,
			.cpu_mask = has_m ? cpu_mask_str : NULL,
			.pattern_file = has_M ? pattern_file : NULL
		};
		int rc = run_loaded_latency_ex(&ll_opts);
		if (rc != 0)
			mlc_exit(rc, NULL);
		break;
	}
	case MODE_C2C_LATENCY: {
		printf("Command line parameters: ");
		for (int i = 1; i < argc; i++)
			printf("%s ", argv[i]);
		printf("\n");
		if (!keep_prefetchers)
			msr_disable_prefetchers(verbose);
		bool c2c_summary =
		    !(has_b || has_c || has_C || has_i || has_r || has_w ||
		      has_x || has_S);
		c2c_latency_options_t c2c_opts = {
			.summary_mode = c2c_summary,
			.has_reader_cpu = has_c,
			.reader_cpu = target_core,
			.has_writer_cpu = has_w,
			.writer_cpu = writer_core,
			.third_cpu = c2c_3rd_core,
			.has_init_cpu = has_i,
			.init_cpu = init_core,
			.has_buf_size = has_b,
			.buf_size_kb = (size_t)buf_size_kb,
			.has_window_size = has_C,
			.window_size_bytes = c2c_window_bytes,
			.has_writer_extra = has_E,
			.writer_extra_bytes = c2c_writer_extra_bytes,
			.reader_extra_bytes = c2c_reader_extra_bytes,
			.clean_hit = has_H,
			.reader_rfo = has_B,
			.do_clflush = c2c_do_clflush,
			.random_access = random_access,
			.random_range = random_range_D,
			.stride = stride,
			.total_iters = c2c_iters_total,
			.has_iters = has_x,
			.verbose = verbose
		};
		int rc =
		    run_c2c_latency_ex(&c2c_opts, helper_get_tsc_freq_ghz());
		if (rc != 0)
			mlc_exit(rc, NULL);
		break;
	}
	case MODE_C2C_BANDWIDTH: {
		printf("Command line parameters: ");
		for (int i = 1; i < argc; i++)
			printf("%s ", argv[i]);
		printf("\n");
		double c2c_banner_mib =
		    has_b ? ((double)buf_size_kb / 1024.0)
			  : (double)DEFAULT_BW_BUF_PER_THREAD_MB;
		printf("\nUsing buffer size of %.3fMiB/thread for reads and an additional %.3fMiB/thread for writes\n",
		       c2c_banner_mib, c2c_banner_mib);
		if (c2c_engine == C2C_ENGINE_MATRIX) {
			if (cpu_is_amd_processor())
				numa_sample_l3_domains(full_l3_matrix);
			else
				numa_reset_l3_sampling();
			if (!keep_prefetchers)
				msr_disable_prefetchers(verbose);
		} else if (!keep_prefetchers) {
			if (c2cbw_pref_mode == 1)
				msr_enable_prefetchers(verbose);
			else if (c2cbw_pref_mode == 2)
				msr_disable_prefetchers(verbose);
		}
		int c2c_simd = has_Y ? 1
				     : (has_Z ? 2
					      : (cpu_has_avx512f()
						     ? 2
						     : (cpu_has_avx2() ? 1 : 0)));
		c2c_bw_options_t c2c_bw_opts = {
			.engine = c2c_engine,
			.writer_str = c2cbw_writer_str,
			.reader_str = c2cbw_reader_str,
			.sharer_str = c2cbw_sharer_str,
			.has_b = has_b,
			.buf_kb = buf_size_kb,
			.has_C = has_C,
			.chunk_kb = (int64_t)(c2c_window_bytes >> 10),
			.time_sec = has_t ? (int)duration_sec : 3,
			.simd_level = c2c_simd,
			.rw_mix = has_W ? traffic_W : 0,
			.hit_clean = has_H,
			.shared_buf = shared_buf,
			.verbose = verbose || c2cbw_verbose,
			.delay = c2cbw_delay,
			.stepped = c2cbw_stepped,
			.partial = c2cbw_partial,
			.ngroups = c2cbw_ngroups,
			.skip_kb = c2cbw_skip_kb,
			.auto_sharer = c2cbw_auto_sharer
		};
		int rc = run_c2c_bandwidth_dispatch(&c2c_bw_opts);
		if (rc != 0)
			mlc_exit(rc, NULL);
		break;
	}
	case MODE_CONCURRENT_BANDWIDTH: {
		printf("Command line parameters: ");
		for (int i = 1; i < argc; i++) {
			printf("%s ", argv[i]);
		}
		printf("\n");
		size_t cold_kb =
		    has_b ? (size_t)buf_size_kb
			  : ((size_t)DEFAULT_BW_BUF_PER_THREAD_MB * 1024ULL);
		double banner_mib = (double)cold_kb / 1024.0;
		printf("\nUsing buffer size of %.3fMiB/thread for reads and an additional %.3fMiB/thread for writes\n",
		       banner_mib, banner_mib);
		if (!keep_prefetchers)
			msr_enable_prefetchers(verbose);
		conc_bw_options_t conc_opts = {
			.cold_kb = cold_kb,
			.hot_kb = conc_hotbuf_kb,
			.hot_pct = conc_hot_pct,
			.duration_sec = has_t ? duration_sec : 3.0,
			.single_thread_per_core = single_thread_per_core,
			.shared_hot_buf = shared_buf,
			.per_thread_bw = per_thread_bw,
			.verbose = verbose,
			.has_numa_node = has_j,
			.numa_node = scan_node,
			.cpu_list = has_k ? cpu_list_str : NULL,
			.cpu_mask = has_m ? cpu_mask_str : NULL
		};
		int rc = run_concurrent_bandwidth(&conc_opts);
		if (rc != 0)
			mlc_exit(rc, NULL);
		break;
	}
	case MODE_MEMORY_SCAN:{
			printf("Command line parameters: --memory_scan \n\n");
			memory_scan_config_t scfg = {
				.numa_node = scan_node,
				.num_threads = total_cpus,
				.scan_gb = 0,
				.bucket_granularity_ns =
				    (histogram_granularity >
				     0) ? histogram_granularity : 10,
				.iterations = 1,
				.use_1gb_pages =
				    (numa_get_hugepage_mode() == HUGEPAGE_1GB),
				.verbose = verbose
			};
			run_memory_bw_scan(&scfg);
			break;
		}
	case MODE_LATENCY_HISTOGRAM:
		printf("Command line parameters: --latency_histogram \n\n");
		if (!keep_prefetchers)
			msr_disable_prefetchers(verbose);
		run_latency_histogram(buf_size_mb,
				      (histogram_granularity >
				       0) ? histogram_granularity : 10);
		break;
	case MODE_DELAY_POINTER_CHASE:{
			printf("Command line parameters: ");
			for (int i = 1; i < argc; i++) {
				printf("%s ", argv[i]);
			}
			printf("\n");
			double banner_mib = has_b ? ((double)buf_size_kb / 1024.0)
						  : (double)DEFAULT_BW_BUF_PER_THREAD_MB;
			printf
			    ("\nUsing buffer size of %.3fMiB/thread for reads and an additional %.3fMiB/thread for writes\n",
			     banner_mib, banner_mib);
			if (!keep_prefetchers)
				msr_disable_prefetchers(verbose);
			dpc_cfg.target_core = target_core;
			char err_buf[256] = { 0 };
			int rc =
			    run_delay_pointer_chase(&dpc_cfg, err_buf,
						    sizeof(err_buf));
			if (rc != 0) {
				mlc_exit(rc, err_buf);
			}
			break;
		}
	case MODE_DEFAULT:
	default:
		/* Complete default Intel MLC test suite run */
		run_latency_matrix(buf_size_mb, random_access,
				   keep_prefetchers, verbose);
		printf("\n");
		sleep(1);

		if (!keep_prefetchers)
			msr_enable_prefetchers(false);
		run_peak_bandwidth(bw_buf_mb, duration_sec,
				   single_thread_per_core);
		printf("\n");
		sleep(1);

		run_bandwidth_matrix(bw_buf_mb, duration_sec,
				     single_thread_per_core, verbose,
				     eff_traffic);
		printf("\n");
		sleep(1);

		run_loaded_latency(bw_buf_mb, duration_sec, random_access,
				   keep_prefetchers, target_core,
				   single_thread_per_core, throughput_only,
				   eff_traffic);
		sleep(1);

		if (!keep_prefetchers)
			msr_disable_prefetchers(false);
		run_c2c_latency_benchmark(helper_get_tsc_freq_ghz());

		if (!legacy_mode) {
			printf("\n");
			sleep(1);
			if (cpu_is_amd_processor())
				numa_sample_l3_domains(full_l3_matrix);
			else
				numa_reset_l3_sampling();
			if (!keep_prefetchers)
				msr_disable_prefetchers(false);
			c2c_bw_options_t def_c2c_opts;
			c2c_bw_options_init(&def_c2c_opts);
			def_c2c_opts.engine = C2C_ENGINE_MATRIX;
			def_c2c_opts.time_sec = has_t ? (int)duration_sec : 3;
			def_c2c_opts.simd_level =
			    has_Y ? 1
				  : (has_Z ? 2
					   : (cpu_has_avx512f()
						  ? 2
						  : (cpu_has_avx2() ? 1 : 0)));
			def_c2c_opts.verbose = verbose;
			run_c2c_bandwidth_dispatch(&def_c2c_opts);
		}
		break;
	}

	mlc_exit(0, NULL);
	return 0;
}
