#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <cpuid.h>
#include <immintrin.h>
#include "helper.h"

#define MAX_TRACKED_CPUS 512

static double g_tsc_freq_ghz = 2.5;
static int g_total_cpus = 0;
static bool g_msr_available = false;
static bool g_prefetchers_disabled = false;
static uint32_t g_msr_register = INTEL_MSR_PREFETCH_CONTROL;
static uint64_t g_disable_mask = INTEL_PREFETCH_DISABLE_MASK;
static uint64_t g_orig_values[MAX_TRACKED_CPUS];
static bool g_cpu_modified[MAX_TRACKED_CPUS];

void helper_init_tsc(void)
{
	struct timespec t_start, t_end;
	uint64_t tsc_start, tsc_end;

	clock_gettime(CLOCK_MONOTONIC_RAW, &t_start);
	tsc_start = __rdtsc();
	usleep(50000);		/* 50 ms */
	clock_gettime(CLOCK_MONOTONIC_RAW, &t_end);
	tsc_end = __rdtsc();

	double elapsed_sec = (t_end.tv_sec - t_start.tv_sec) +
	    (t_end.tv_nsec - t_start.tv_nsec) * 1e-9;
	g_tsc_freq_ghz = ((double)(tsc_end - tsc_start) / elapsed_sec) * 1e-9;
	if (g_tsc_freq_ghz < 0.5)
		g_tsc_freq_ghz = 2.5;
}

double helper_get_tsc_freq_ghz(void)
{
	return g_tsc_freq_ghz;
}

static void signal_handler(int sig)
{
	(void)sig;
	msr_restore_prefetchers();
	_exit(1);
}

static bool read_msr(int cpu, uint32_t reg, uint64_t *val)
{
	char path[64];
	snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return false;
	ssize_t ret = pread(fd, val, sizeof(uint64_t), reg);
	close(fd);
	return ret == sizeof(uint64_t);
}

static bool write_msr(int cpu, uint32_t reg, uint64_t val)
{
	char path[64];
	snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return false;
	ssize_t ret = pwrite(fd, &val, sizeof(uint64_t), reg);
	close(fd);
	return ret == sizeof(uint64_t);
}

bool msr_init(int total_cpus)
{
	g_total_cpus =
	    (total_cpus > MAX_TRACKED_CPUS) ? MAX_TRACKED_CPUS : total_cpus;
	memset(g_orig_values, 0, sizeof(g_orig_values));
	memset(g_cpu_modified, 0, sizeof(g_cpu_modified));

	unsigned int eax, ebx, ecx, edx;
	if (__get_cpuid(0, &eax, &ebx, &ecx, &edx)) {
		char vendor[13];
		memcpy(vendor, &ebx, 4);
		memcpy(vendor + 4, &edx, 4);
		memcpy(vendor + 8, &ecx, 4);
		vendor[12] = '\0';

		if (strcmp(vendor, "AuthenticAMD") == 0) {
			g_msr_register = AMD_ZEN3_MSR_PREFETCH_CONTROL;
			g_disable_mask = 0x01ULL;
		} else {
			g_msr_register = INTEL_MSR_PREFETCH_CONTROL;
			g_disable_mask = INTEL_PREFETCH_DISABLE_MASK;
		}
	}

	uint64_t test_val;
	if (read_msr(0, g_msr_register, &test_val)) {
		g_msr_available = true;
	} else {
		g_msr_available = false;
	}

	atexit(msr_restore_prefetchers);
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	return g_msr_available;
}

bool msr_is_available(void)
{
	return g_msr_available;
}

int msr_disable_prefetchers(bool verbose)
{
	if (!g_msr_available || g_prefetchers_disabled)
		return 0;

	int modified_count = 0;
	for (int cpu = 0; cpu < g_total_cpus; cpu++) {
		uint64_t val = 0;
		if (read_msr(cpu, g_msr_register, &val)) {
			g_orig_values[cpu] = val;
			uint64_t new_val = val | g_disable_mask;
			if (write_msr(cpu, g_msr_register, new_val)) {
				g_cpu_modified[cpu] = true;
				modified_count++;
			}
		}
	}

	if (modified_count > 0) {
		g_prefetchers_disabled = true;
		if (verbose) {
			printf
			    ("Hardware prefetchers successfully disabled on %d CPU(s) (MSR 0x%X).\n",
			     modified_count, g_msr_register);
		}
	} else {
		if (verbose) {
			printf
			    ("*** Unable to modify prefetchers (try executing 'modprobe msr' as root)\n");
		}
	}

	return modified_count;
}

void msr_restore_prefetchers(void)
{
	if (!g_prefetchers_disabled)
		return;

	for (int cpu = 0; cpu < g_total_cpus; cpu++) {
		if (g_cpu_modified[cpu]) {
			write_msr(cpu, g_msr_register, g_orig_values[cpu]);
			g_cpu_modified[cpu] = false;
		}
	}
	g_prefetchers_disabled = false;
}

void print_usage(void)
{
	static const char usage_str[] =
		"\n"
		"USAGE: Default and recommended usage is to just run the binary \"./mlc\". For specific analysis, there are six modes:\n"
		"./mlc --latency_matrix \t\t prints a matrix of local and cross-socket memory latencies\n"
		"./mlc --bandwidth_matrix \t prints a matrix of local and cross-socket memory bandwidths\n"
		"./mlc --peak_injection_bandwidth \t\t prints peak memory bandwidths of the platform for various read-write ratios\n"
		"./mlc --idle_latency \t\t prints the idle memory latency of the platform\n"
		"./mlc --loaded_latency \t\t prints the loaded memory latency of the platform\n"
		"./mlc --c2c_latency \t\t prints the cache to cache data transfer latency of the platform\n"
		"./mlc --c2c_bandwidth \t\t prints cross-L3 cache-to-cache read bandwidth\n"
		"./mlc --concurrent_bandwidth \t prints concurrent hot/cold memory bandwidth\n"
		"./mlc --delay_pointer_chase \t measures pointer-chase latency with a configurable delay sub-chain\n"
		"\t  Optional: --dchainN (single chain_len), --dsweepN (sweep 1..N, default 64),\n"
		"\t            --dstrideN (slot stride, default 1), --dslotsN (slot count, default 31250000) or -b<size>,\n"
		"\t            --ditersN (iterations, default 10000000), --dcore_mhzN (core freq for ratio),\n"
		"\t            --doverheadN (per-node overhead cycles for adjusted output, default 4)\n"
		"./mlc --c2c_readerbw_matrix \t prints an NxN matrix of cross-L3 reader bandwidth\n"
		"./mlc --memory_bandwidth_scan \t\t prints memory bandwidth across entire memory for each 1GB address range\n"
		"./mlc --help \t\t prints this usage description and exits\n"
		"\n"
		"./mlc [-e] [-r] [-X] [-v]\n"
		"\t -e h/w prefetcher state will not be modified\n"
		"\t -r random access to get latencies - needed when prefetchers can't be disabled by MLC\n"
		"\t -X only one thread from each core used\n"
		"\t -v verbose printout\n"
		"\n"
		"For custom analysis (example: getting the latency matrix with a larger buffer size), specify flags to override the defaults. Each usage mode supports the following flags:\n"
		"./mlc --latency_matrix [-a] [-bn] [-Dn] [-e] [-ln] [-L|-h] [-tn] [-r] [-tn] [-xn] [-X]\n"
		"./mlc --idle_latency [-bn] [-cn] [-Dn] [-e] [-fn] [-in] [-jn] [-Jn] [-ln] [-L|-h] [-r] [-tn] [-xn] [-X]\n"
		"./mlc --loaded_latency [-bn] [-B] [-cn] [-dn] [-Dn] [-e] [-g<filename>] [-in] [-I] [-jn] [-kn|-mn] [-Kn] [-ln]\n"
		"\t[-L|-h] [-M<filename>] [-nn] [-o<filename] [-P] [-Q] [-r] [-R] [-tn] [-T] [-u] [-U] [-v] [-Wn] [-X] [-Y] [-Z]\n"
		"./mlc --bandwidth_matrix [-bn] [-e] [-ln]  [-L|-h] [-M<filename>] [-R] [-tn] [-Wn] [-X] [-Y] [-Z]\n"
		"./mlc --peak_injection_bandwidth [-bn] [-e] [-kn|-mn] [-ln] [-L|-h] [-M<filename>] [-tn] [-X] [-Y] [-Z]\n"
		"./mlc --max_bandwidth [-bn] [-e] [-kn|-mn] [-ln] [-L|-h] [-M<filename>] [-tn] [-X] [-Y] [-Z]\n"
		"./mlc --c2c_latency [-bn] [-cn] [-Cn] [-Dn] {-En] [-Gn] [-H] [-in] [-L|-h] [-r] [-Sn] [-wn] [-xn]\n"
		"./mlc --c2c_bandwidth [--c2cbw_reader=cpulist] [--c2cbw_writer=cpulist] [--c2cbw_sharer=cpulist]\n"
		"\t[--c2cbw_ngroups=n] [--c2cbw_delay=n] [--c2cbw_stepped] [--c2cbw_pref|--c2cbw_nopref]\n"
		"\t[--c2cbw_partial] [--c2cbw_auto_sharer] [--c2cbw_skip=KB] [--c2cbw_verbose]\n"
		"\t[-bn] [-Cn] [-e] [-H] [-R] [-tn] [-Z]\n"
		"./mlc --c2c_readerbw_matrix [-bn] [-Cn] [-e] [-H] [-tn]\n"
		"./mlc --concurrent_bandwidth [-bn] [-kn|-mn] [-tn] [--conc_hotbufn] [--conc_hot_pctn] [-X]\n"
		"./mlc --delay_pointer_chase [-bn] [--dchainn] [--dsweepn] [--dstriden] [--dslotsn] [--ditersn] [--dcore_mhzn] [--doverheadn]\n"
		"./mlc --memory_bandwidth_scan [-jn] [-tn] [-Sn] [-qn] [-nn]\n"
		"\n"
		"  -a measure idle latencies on all available cpus; if -X is also specified, then only one thread in each core will be used\n"
		"  -b buffer size in KiB (default=100000)\n"
		"  -B print per-thread throughput\n"
		"  -c affinitize the latency measurement thread to core #n\n"
		"  -C window size in KB for c2c_latency(default=2000)\n"
		"  -d delay cycles injected between requests to memory (default-0), higher value lowers bandwidth\n"
		"  -D random access range for latency thread (default=4096)\n"
		"  -e do not modify the h/w prefetcher states\n"
		"  -E read extra n KB lines in writer thread in c2c_latency(default=0)\n"
		"  -f latency histogram bucket width as a power of two exponent, so the bucket\n"
		"  \t is 1 << n cycles wide (n=0 gives 1 cycle, n=6 gives 64). Maximum is 30\n"
		"  -g <input-file-name> specify input file with delays to inject, for loaded latency measurement\n"
		"  -G buffer size in KB for extra reads in reader thread in c2c_latency\n"
		"  -h HugePage (1GB) are to be used (on windows only for DAX file mapping w/ 1GB page)\n"
		"  -H Test cache to cache transfer latency for clean line transfers\n"
		"  -i initialize memory from core #n (determines where requested memory resides)\n"
		"  -I Check for data integrity on reads - valid only for all reads in loaded_latency mode\n"
		"  -j initialize memory from numa node #n - if specified, this overrides -i\n"
		"  -J specify a directory in which files for mmap will be created (by default no files are created) this option is mutually exclusive with -j\n"
		"  -k specify a list of core# (use '-' for range e.g 3-6,9-13,19,20 \n"
		"  -K option to control partial load/store to 64B lines: -K0 forces full\n"
		"     64 byte line access, -K1 and above force partial access. Left alone,\n"
		"     partial access is chosen automatically on parts that support specItoM\n"
		"  -l stride length in bytes (default=64)\n"
		"  -L large pages (2MB) are to be used (assuming they have been enabled)\n"
		"  -m mask in hex value of CPUs to run the throughput threads for bandwidth measurement. It is recommended -k option is used instead\n"
		"\tlatency thread always runs on core 0 and this should be excluded from the mask\n"
		"  -M <filename> initialize the memory with specific patterns \n"
		"  -n random access range for random bandwidth generation. In --memory_bandwidth_scan mode\n"
		"  \t this specifies the numa node# where the cpu threads accessing memory will be running.\n"
		"  -o <input-file-name> specify input file with options for per-thread allocation\n"
		"   \t each row should contain thread_id, read/write ratio, random/seq, buffer size in KB, DDR4/pmem, remote node id\n"
		"  -O add start and stop timestamps to workload\n"
		"  -p list of the cores separated by comma, where a thread will be run to prevent the corresponding sockets from transitioning to package sleep states\n"
		"  -q # of iterations of each 1GB memory region where B/W is measured. Default is 10\n"
		"  -P CLFLUSH used to evict stores to persistent memory\n"
		"  -Q CLFLUSH used to evict stores to any memory\n"
		"  -r random access reads for latency thread\n"
		"  -R read-only load generated, if this is used, -W option should NOT be used \n"
		"  -s specify a obj neame that can be triggerd by external application to synchornize the loaded latency traffic. ex: -s/test_flag \n"
		"  -S affinitize 3rd thread in c2c_latency test to core #n. In --memory_bandwidth_scan mode\n"
		"  \t this indicates the bandwidth range for each bucket in terms of MB/sec. Default is 5000\n"
		"  -t time in seconds (default=2).  In --memory_bandwidth_scan mode\n"
		"  \t this specifies the # of cpu threads that will be used to access memory (default is all threads on that numa node)\n"
		"  -T throughput threads only, no latency measurement, -m or -k option can select all CPUs including core 0\n"
		"  -u same buffer is shared by all load generation threads in loaded_latency tests\n"
		"  -U random access for bandwidth generation (default: off, only sequential access)\n"
		"  -v print verbose output\n"
		"  -w affinitize hit/hitm writer thread to core #n\n"
		"  -Wn where n means\n"
		"\t2  - 2:1 read-write ratio\n"
		"\t3  - 3:1 read-write ratio\n"
		"\t4  - 3:2 read-write ratio\n"
		"\t5  - 1:1 read-write ratio\n"
		"\t6  - 0:1 read-Non Temporal Write ratio\n"
		"\t7  - 2:1 read-Non Temporal Write ratio\n"
		"\t8  - 1:1 read-Non Temporal Write ratio\n"
		"\t9  - 3:1 read-Non Temporal Write ratio\n"
		"\t10 - 2:1 read-Non Temporal Write ratio (stream triad-like)\n"
		"\t\t Same as -W7 but the 2 reads are from 2 different buffers\n"
		"\t11 - 3:1 read-Write ratio (stream triad-like with RFO)\n"
		"\t\t Same as -W3 but the 2 reads are from 2 different buffers\n"
		"\t12 - 4:1 read-Write ratio\n"
		"\t21 - 100% read with 2 addr streams - valid with only -o option\n"
		"\t23 - 3:1 read-write ratio with 2 addr streams - valid with only -o option\n"
		"\t27 - 2:1 read-NT write with 2 addr streams - valid with only -o option\n"
		"  -x number of iterations in #n millions - if specified, -t should not be specified (-x0 is overloaded case for 1 iteration)\n"
		"  -X use only 1 hyper-thread per core for bandwidth measurements\n"
		"  -Y use AVX2 32-byte load/store instructions\n"
		"  -Z use AVX-512 64-byte load/store instructions\n"
		"  -Zn same, but issue loads in bursts of n before the injected delay.\n"
		"\t n must be 1, 2 or 4, and applies only to 100% read traffic (-W1)\n"
		"\n"
		"c2c_bandwidth sub-options:\n"
		"  --c2cbw_reader=cpulist   specify reader core list (e.g. 168-223)\n"
		"  --c2cbw_writer=cpulist   specify writer core list (e.g. 0|56 for multi-group)\n"
		"  --c2cbw_sharer=cpulist   specify sharer core list (e.g. 1-12|57-68)\n"
		"  --c2cbw_ngroups=n        number of writer/reader groups (default 1)\n"
		"  --c2cbw_delay=n          delay cycles between writer iterations\n"
		"  --c2cbw_stepped          stepped mode: writers warm cache, then readers read\n"
		"  --c2cbw_pref             force-enable h/w prefetchers for c2c test\n"
		"  --c2cbw_nopref           force-disable h/w prefetchers for c2c test\n"
		"  --c2cbw_partial          use partial writes (single byte) instead of full-line\n"
		"  --c2cbw_auto_sharer      auto-place sharers on writer's L3 cluster\n"
		"  --c2cbw_skip=KB          skip first KB of buffer per epoch (for warmup)\n"
		"  --c2cbw_verbose          print per-epoch timing details\n"
		"\n"
		"  c2c_bandwidth examples:\n"
		"    # 1 group: 1 writer on core 0, 12 sharers on cores 1-12, 56 readers on cores 168-223\n"
		"    ./mlc --c2c_bandwidth --c2cbw_writer=0 --c2cbw_sharer=1-12 --c2cbw_reader=168-223 -t10\n"
		"    # 2 groups: writers on cores 0,56; sharers on 1-12,57-68; readers split across 168-195,196-223\n"
		"    ./mlc --c2c_bandwidth --c2cbw_ngroups=2 '--c2cbw_writer=0|56' \\\n"
		"        '--c2cbw_sharer=1-12|57-68' '--c2cbw_reader=168-195|196-223' -t10\n"
		"    # 3 groups with auto-sharer placement and verbose output\n"
		"    ./mlc --c2c_bandwidth --c2cbw_ngroups=3 '--c2cbw_writer=0|56|112' \\\n"
		"        '--c2cbw_reader=168-186|187-205|206-223' --c2cbw_auto_sharer --c2cbw_verbose -t10\n"
		"\n"
		"concurrent_bandwidth sub-options:\n"
		"  --conc_hotbuf=n          hot buffer size in KB\n"
		"  --conc_hot_pct=n         percentage of accesses to hot buffer (0-100)\n"
		"\n"
		"loaded_latency modifiers:\n"
		"  --fastdelay<d1,d2,...>    cycle through delay values (inline list)\n"
		"  --fastdelayfile<path>    load delay values from file\n"
		"  --fastdelayinterval<ms>  time per delay value (default: based on -t)\n"
		"  --fastdelayverbose       print fastdelay progress\n"
		"  --lat_hist_max_us=n      set latency histogram upper bound to n microseconds (idle_latency + -f)\n"
		"  --lat_hist_max_cycles=n  set latency histogram upper bound to n base-frequency cycles\n"
		"\n"
		"L3-domain modifiers (combine with a mode above):\n"
		"  --per_llc_cluster        run --loaded_latency or --bandwidth_matrix per-L3-cluster\n"
		"  --legacy_mode            skip the extended L3-domain tests in a default run\n"
		"  --full_l3_matrix         measure every pair of L3 domains in a default run, rather\n"
		"\t                   than a representative sample. Only parts with a large\n"
		"\t                   number of L3 domains are sampled; the rest already do\n"
		"\t                   the full matrix and this changes nothing for them.\n"
		"\n"
		"Note that the tool uses the following terminology:\n"
		"'core' denotes logical core\n"
		"'socket' denotes processor socket/package/node \n"
		"'memory node' denotes a distinct memory NUMA region, identifiable using numactl APIs\n"
	;
	fputs(usage_str, stderr);
}
