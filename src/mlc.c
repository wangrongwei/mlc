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

static int write_proc_int(const char *path, int val)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	int ret = fprintf(f, "%d", val);
	fclose(f);
	return (ret > 0) ? 0 : -1;
}

static void restore_numa_balancing(void)
{
	if (scan_delay_modified && orig_scan_delay >= 0) {
		write_proc_int("/proc/sys/kernel/numa_balancing_scan_delay_ms", orig_scan_delay);
		scan_delay_modified = false;
	}
	if (numa_balancing_modified && orig_numa_balancing >= 0) {
		write_proc_int("/proc/sys/kernel/numa_balancing", orig_numa_balancing);
		numa_balancing_modified = false;
	}
}

static void manage_numa_balancing(void)
{
	orig_scan_delay = read_proc_int("/proc/sys/kernel/numa_balancing_scan_delay_ms");
	orig_numa_balancing = read_proc_int("/proc/sys/kernel/numa_balancing");
	if (orig_numa_balancing > 0) {
		if (write_proc_int("/proc/sys/kernel/numa_balancing", 0) == 0) {
			numa_balancing_modified = true;
		}
	}
}

static void mlc_exit(int status, const char *msg)
{
	if (msg && *msg) {
		fprintf(stderr, "%s\n", msg);
	}
	msr_restore_prefetchers();
	restore_numa_balancing();
	exit(status);
}

static void mlc_signal_handler(int sig)
{
	signal(sig, SIG_IGN);
	msr_restore_prefetchers();
	restore_numa_balancing();
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
	return (w >= 1 && w <= 12) || w == 21 || w == 23 || w == 27;
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

	helper_init_tsc();
	numa_init_topology();
	int total_cpus = numa_get_total_cpus();
	int num_nodes = numa_get_num_nodes();
	msr_init(total_cpus);

	size_t buf_size_mb = DEFAULT_LATENCY_BUF_MB;
	double duration_sec = DEFAULT_TIME_SEC;
	bool random_access = false;
	bool verbose = false;
	bool keep_prefetchers = false;
	bool all_cores_latency = false;
	bool single_thread_per_core = false;
	int target_core = -1;
	int writer_core = -1;
	int c2c_3rd_core = -1;
	const char *pstate_cores = NULL;
	int histogram_granularity = 10;
	int scan_node = 0;
	const char *ddio_shm_name = "/mlc_ddio_shm";
	bool use_1gb_pages = false;
	bool legacy_mode = false;
	int stride = 64;
	int traffic_W = 2;
	bool has_W = false;
	bool has_R = false;
	bool has_m = false;
	bool has_k = false;
	bool has_o = false;
	bool has_j = false;
	bool has_n = false;
	bool has_U = false;
	bool has_x = false;
	int iters_x = 0;
	bool has_fastdelay = false;

	enum {
		MODE_DEFAULT,
		MODE_IDLE_LATENCY,
		MODE_LATENCY_MATRIX,
		MODE_BANDWIDTH_MATRIX,
		MODE_PEAK_BANDWIDTH,
		MODE_LOADED_LATENCY,
		MODE_C2C_LATENCY,
		MODE_C2C_BANDWIDTH,
		MODE_CONCURRENT_BANDWIDTH,
		MODE_MEMORY_SCAN,
		MODE_LATENCY_HISTOGRAM,
		MODE_DDIO,
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
			} else if (strcmp(opt, "peak_injection_bandwidth") == 0 ||
				   strcmp(opt, "max_bandwidth") == 0) {
				SET_MODE(MODE_PEAK_BANDWIDTH);
			} else if (strcmp(opt, "peak_bandwidth") == 0) {
				fprintf(stderr, "peak_bandwidth name has been changed. Use peak_injection_bandwidth instead\n");
				mlc_exit(22, NULL);
			} else if (strcmp(opt, "loaded_latency") == 0) {
				SET_MODE(MODE_LOADED_LATENCY);
			} else if (strcmp(opt, "c2c_latency") == 0) {
				SET_MODE(MODE_C2C_LATENCY);
			} else if (strcmp(opt, "c2c_bandwidth") == 0 ||
				   strcmp(opt, "c2c_bandwidth_matrix") == 0 ||
				   strcmp(opt, "c2c_readerbw") == 0 ||
				   strcmp(opt, "c2c_reader_bw") == 0 ||
				   strcmp(opt, "c2c_readerbw_matrix") == 0 ||
				   strcmp(opt, "c2c_reader_bw_matrix") == 0 ||
				   strcmp(opt, "c2c_bandwidth_pipelined") == 0 ||
				   strncmp(opt, "c2cbw_", 6) == 0) {
				SET_MODE(MODE_C2C_BANDWIDTH);
			} else if (strcmp(opt, "concurrent_bandwidth") == 0) {
				SET_MODE(MODE_CONCURRENT_BANDWIDTH);
			} else if (strcmp(opt, "memory_scan") == 0 ||
				   strcmp(opt, "memory_bandwidth_scan") == 0) {
				SET_MODE(MODE_MEMORY_SCAN);
			} else if (strcmp(opt, "delay_pointer_chase") == 0) {
				SET_MODE(MODE_IDLE_LATENCY);
			} else if (strcmp(opt, "latency_histogram") == 0) {
				SET_MODE(MODE_LATENCY_HISTOGRAM);
			} else if (strcmp(opt, "ddio") == 0) {
				SET_MODE(MODE_DDIO);
			} else if (strcmp(opt, "help") == 0) {
				SET_MODE(MODE_HELP);
			} else if (strcmp(opt, "legacy_mode") == 0) {
				legacy_mode = true;
			} else if (strcmp(opt, "full_l3_matrix") == 0) {
				/* full L3 matrix measurement in default run */
			} else if (strcmp(opt, "per_llc_cluster") == 0) {
				/* run per-L3-cluster measurements */
			} else if (strncmp(opt, "scan_node=", 10) == 0) {
				scan_node = atoi(opt + 10);
			} else if (strncmp(opt, "ddiomapname=", 12) == 0) {
				ddio_shm_name = opt + 12;
			} else if (strncmp(opt, "fastdelay", 9) == 0) {
				has_fastdelay = true;
			} else if (strncmp(opt, "lat_hist_max_us", 15) == 0) {
				/* lat_hist_max_us */
			} else if (strncmp(opt, "lat_hist_max_cycles", 19) == 0) {
				/* lat_hist_max_cycles */
			} else if (strncmp(opt, "conc_hotbuf", 11) == 0) {
				/* conc_hotbuf */
			} else if (strncmp(opt, "conc_hot_pct", 12) == 0) {
				const char *val_p = opt + 12;
				if (*val_p == '=') val_p++;
				int pct = atoi(val_p);
				int valid_pcts[] = {0, 10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 87, 90, 93, 100};
				bool ok = false;
				for (size_t p = 0; p < sizeof(valid_pcts)/sizeof(valid_pcts[0]); p++) {
					if (pct == valid_pcts[p]) { ok = true; break; }
				}
				if (!ok) {
					fprintf(stderr, "--conc_hot_pct must be 0, 10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 87, 90, 93, or 100\n");
					mlc_exit(22, NULL);
				}
			} else if (strcmp(opt, "count_only_online_CPUs") == 0 ||
				   strcmp(opt, "print_cpuid") == 0 ||
				   strcmp(opt, "physaddr_dump") == 0 ||
				   strncmp(opt, "bwscan_", 7) == 0 ||
				   strncmp(opt, "dchain", 6) == 0 ||
				   strncmp(opt, "dstride", 7) == 0 ||
				   strncmp(opt, "dsweep", 6) == 0 ||
				   strncmp(opt, "dslots", 6) == 0 ||
				   strncmp(opt, "diters", 6) == 0 ||
				   strncmp(opt, "dcore_mhz", 9) == 0 ||
				   strncmp(opt, "doverhead", 9) == 0 ||
				   strncmp(opt, "delayunroll", 11) == 0 ||
				   strncmp(opt, "ddio_", 5) == 0) {
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
			case 'B':
				check_arg_no_val('B', val_str);
				break;
			case 'b':
				check_arg_has_val('b', val_str);
				{
					int kb = atoi(val_str);
					if (kb > 0)
						buf_size_mb = kb / 1024;
				}
				break;
			case 'c':
				check_arg_has_val('c', val_str);
				target_core = atoi(val_str);
				if (target_core < 0 || target_core >= total_cpus) {
					fprintf(stderr, "Minimum value for core number is 0 and maximum value is %d\nCore number must be in this range! \n", total_cpus - 1);
					mlc_exit(22, NULL);
				}
				break;
			case 'C':
				check_arg_has_val('C', val_str);
				break;
			case 'd':
				check_arg_has_val('d', val_str);
				break;
			case 'D':
				check_arg_has_val('D', val_str);
				break;
			case 'e':
				check_arg_no_val('e', val_str);
				keep_prefetchers = true;
				break;
			case 'E':
				check_arg_has_val('E', val_str);
				break;
			case 'f':
				check_arg_has_val('f', val_str);
				{
					int exp = atoi(val_str);
					if (exp >= 0 && exp <= 30)
						histogram_granularity = 1 << exp;
				}
				break;
			case 'g':
				check_arg_has_val('g', val_str);
				break;
			case 'G':
				check_arg_has_val('G', val_str);
				histogram_granularity = atoi(val_str);
				break;
			case 'h':
				check_arg_no_val('h', val_str);
				use_1gb_pages = true;
				numa_set_hugepage_mode(HUGEPAGE_1GB);
				break;
			case 'H':
				check_arg_no_val('H', val_str);
				break;
			case 'i':
				check_arg_has_val('i', val_str);
				break;
			case 'I':
				check_arg_no_val('I', val_str);
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
				break;
			case 'K':
				check_arg_has_val('K', val_str);
				break;
			case 'l':
				check_arg_has_val('l', val_str);
				stride = atoi(val_str);
				if (stride <= 0) {
					fprintf(stderr, "Stride length must be greater than zero. -l is the distance in bytes between consecutive accesses, and the default is 64. Exiting..\n");
					mlc_exit(22, NULL);
				}
				break;
			case 'L':
				check_arg_no_val('L', val_str);
				numa_set_hugepage_mode(HUGEPAGE_2MB);
				break;
			case 'm':
				check_arg_has_val('m', val_str);
				has_m = true;
				break;
			case 'M':
				check_arg_has_val('M', val_str);
				break;
			case 'n':
				check_arg_has_val('n', val_str);
				has_n = true;
				scan_node = atoi(val_str);
				break;
			case 'o':
				check_arg_has_val('o', val_str);
				has_o = true;
				break;
			case 'O':
				check_arg_no_val('O', val_str);
				break;
			case 'p':
				check_arg_has_val('p', val_str);
				pstate_cores = val_str;
				break;
			case 'P':
				check_arg_no_val('P', val_str);
				break;
			case 'q':
				check_arg_has_val('q', val_str);
				break;
			case 'Q':
				check_arg_no_val('Q', val_str);
				break;
			case 'r':
				check_arg_no_val('r', val_str);
				random_access = true;
				break;
			case 'R':
				check_arg_no_val('R', val_str);
				has_R = true;
				break;
			case 's':
				check_arg_has_val('s', val_str);
				break;
			case 'S':
				check_arg_has_val('S', val_str);
				c2c_3rd_core = atoi(val_str);
				if (c2c_3rd_core < 0 || c2c_3rd_core >= total_cpus) {
					fprintf(stderr, "-S: minimum value for core number is 0 and maximum value is %d\nCore number must be in this range! \n", total_cpus - 1);
					mlc_exit(22, NULL);
				}
				break;
			case 't':
				check_arg_has_val('t', val_str);
				duration_sec = atoi(val_str);
				if (duration_sec <= 0) {
					fprintf(stderr, "Time to run must be greater than zero!\n");
					mlc_exit(22, NULL);
				}
				break;
			case 'T':
				check_arg_no_val('T', val_str);
				break;
			case 'u':
				check_arg_no_val('u', val_str);
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
				iters_x = atoi(val_str);
				if (iters_x < 0) {
					fprintf(stderr, "Number of iterations should be positive!\n");
					mlc_exit(22, NULL);
				}
				break;
			case 'X':
				check_arg_no_val('X', val_str);
				single_thread_per_core = true;
				break;
			case 'Y':
			case 'Z':
				break;
			case 'z':
				check_arg_no_val('z', val_str);
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
	(void)traffic_W;
	if (has_m && has_k) {
		fprintf(stderr, "Only either -m or -k option can be specified.  Exiting...\n");
		mlc_exit(22, NULL);
	}
	if (single_thread_per_core && (has_m || has_o || has_k)) {
		fprintf(stderr, "-X can't be specified if -m or -o or -k is also specified! Exiting...\n");
		mlc_exit(22, NULL);
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
	if (has_n && !has_U && mode != MODE_MEMORY_SCAN) {
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

	check_birch_stream_platform();
	check_hugepages(use_1gb_pages, num_nodes);
	manage_numa_balancing();

	if (pstate_cores) {
		create_pstate_threads_on_other_cores(pstate_cores);
	}

	if (verbose) {
		printf("Detected %d logical CPUs across %d NUMA node(s).\n",
		       total_cpus, num_nodes);
		printf("Calibrated TSC frequency: %.2f GHz\n",
		       helper_get_tsc_freq_ghz());
		printf("Hardware MSR interface: %s\n\n",
		       msr_is_available()? "Available" : "Not accessible");
	}

	if (!keep_prefetchers) {
		msr_disable_prefetchers(verbose);
	}

	if (all_cores_latency) {
		run_idle_latency_all_cores(buf_size_mb, random_access,
					   single_thread_per_core,
					   keep_prefetchers);
		if (pstate_cores)
			stop_pstate_threads();
		mlc_exit(0, NULL);
	}

	switch (mode) {
	case MODE_HELP:
		print_usage();
		break;
	case MODE_IDLE_LATENCY:
		if (buf_size_mb == DEFAULT_LATENCY_BUF_MB) {
			buf_size_mb = 2000;
		}
		if (target_core >= 0) {
			run_idle_latency_core(target_core, buf_size_mb,
					      random_access, keep_prefetchers);
		} else {
			run_idle_latency(buf_size_mb, random_access,
					 keep_prefetchers);
		}
		break;
	case MODE_LATENCY_MATRIX:
		printf("Command line parameters: --latency_matrix \n\n");
		printf("Using buffer size of %.3fMiB\n", (double)buf_size_mb);
		run_latency_matrix(buf_size_mb, random_access,
				   keep_prefetchers);
		break;
	case MODE_BANDWIDTH_MATRIX:
		printf("Command line parameters: --bandwidth_matrix \n\n");
		printf
		    ("Using buffer size of %.3fMiB/thread for reads and an additional %.3fMiB/thread for writes\n",
		     (double)DEFAULT_BW_BUF_PER_THREAD_MB,
		     (double)DEFAULT_BW_BUF_PER_THREAD_MB);
		run_bandwidth_matrix(DEFAULT_BW_BUF_PER_THREAD_MB,
				     duration_sec);
		break;
	case MODE_PEAK_BANDWIDTH:
		printf
		    ("Command line parameters: --peak_injection_bandwidth \n\n");
		printf
		    ("Using buffer size of %.3fMiB/thread for reads and an additional %.3fMiB/thread for writes\n\n",
		     (double)DEFAULT_BW_BUF_PER_THREAD_MB,
		     (double)DEFAULT_BW_BUF_PER_THREAD_MB);
		run_peak_bandwidth(DEFAULT_BW_BUF_PER_THREAD_MB, duration_sec);
		break;
	case MODE_LOADED_LATENCY:
		printf("Command line parameters: --loaded_latency \n\n");
		printf
		    ("Using buffer size of %.3fMiB/thread for reads and an additional %.3fMiB/thread for writes\n\n",
		     (double)DEFAULT_BW_BUF_PER_THREAD_MB,
		     (double)DEFAULT_BW_BUF_PER_THREAD_MB);
		run_loaded_latency(DEFAULT_BW_BUF_PER_THREAD_MB, duration_sec,
				   random_access, keep_prefetchers);
		break;
	case MODE_C2C_LATENCY:
		printf("Command line parameters: --c2c_latency \n\n");
		run_c2c_latency_benchmark(helper_get_tsc_freq_ghz());
		break;
	case MODE_C2C_BANDWIDTH:
		printf("Command line parameters: --c2c_bandwidth \n\n");
		c2c_bandwidth_main(argc, argv, duration_sec);
		break;
	case MODE_CONCURRENT_BANDWIDTH:
		printf("Command line parameters: --concurrent_bandwidth \n\n");
		printf
		    ("Using buffer size of 100.000MiB/thread for reads and an additional 100.000MiB/thread for writes\n\n");
		run_concurrent_bandwidth(102400, 4096, 75, duration_sec);
		break;
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
		run_latency_histogram(buf_size_mb,
				      (histogram_granularity >
				       0) ? histogram_granularity : 10);
		break;
	case MODE_DDIO:
		printf("Command line parameters: --ddio \n\n");
		run_ddio_benchmark(ddio_shm_name, duration_sec);
		break;
	case MODE_DEFAULT:
	default:
		/* Complete default Intel MLC test suite run */
		run_latency_matrix(buf_size_mb, random_access,
				   keep_prefetchers);
		printf("\n");
		sleep(1);

		run_peak_bandwidth(DEFAULT_BW_BUF_PER_THREAD_MB, duration_sec);
		printf("\n");
		sleep(1);

		run_bandwidth_matrix(DEFAULT_BW_BUF_PER_THREAD_MB,
				     duration_sec);
		printf("\n");
		sleep(1);

		run_loaded_latency(DEFAULT_BW_BUF_PER_THREAD_MB, duration_sec,
				   random_access, keep_prefetchers);
		printf("\n");
		sleep(1);

		run_c2c_latency_benchmark(helper_get_tsc_freq_ghz());

		if (!legacy_mode) {
			printf("\n");
			sleep(1);
			c2c_bandwidth_main(argc, argv, duration_sec);
		}
		break;
	}

	if (pstate_cores) {
		stop_pstate_threads();
	}

	mlc_exit(0, NULL);
	return 0;
}
