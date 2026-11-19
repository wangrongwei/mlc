#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <glob.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <cpuid.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include "allocation.h"

#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif

#ifndef MPOL_MF_STRICT
#define MPOL_MF_STRICT 1
#endif

static int g_total_cpus = 0;
static int g_conf_cpus = 0;
static cpu_topology_entry_t g_cpus[MAX_CPUS];
static int g_hwid_to_osid[MAX_CPUS];

static int g_num_sockets = 0;
static socket_info_t g_sockets[MAX_SOCKETS];

static int g_num_nodes = 0;
static numa_node_info_t g_nodes[MAX_NUMA_NODES];

static int g_num_l3_clusters = 0;
static l3_cluster_info_t g_l3_clusters[MAX_L3_CLUSTERS];
static bool g_l3_fallback_used = false;
static bool g_l3_sampled_active = false;
static int g_num_sampled_l3 = 0;
static int g_sampled_l3_ids[MAX_L3_CLUSTERS];

static bool g_numa_mode = false;
static bool g_insufficient_cores_for_busy = false;
static bool g_is_amd = false;
static bool g_is_xeon_atom = false;
static cpu_cache_info_t g_cache_info = {
	.l1i_type = 2,
	.l1i_bytes = 32 * 1024,
	.l1d_type = 1,
	.l1d_bytes = 32 * 1024,
	.l2_type = 3,
	.l2_bytes = 1024 * 1024,
	.l3_type = 3,
	.l3_bytes = 16 * 1024 * 1024
};

static int cmp_int_asc(const void *a, const void *b)
{
	int va = *(const int *)a;
	int vb = *(const int *)b;
	return (va > vb) - (va < vb);
}

static uint32_t ceil_log2_u32(uint32_t v)
{
	if (v <= 1)
		return 0;
	uint32_t shift = 0;
	uint32_t p = 1;
	while (p < v && shift < 31) {
		p <<= 1;
		shift++;
	}
	return shift;
}

static bool read_sysfs_int(const char *path, int *out_val)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	int v = 0;
	bool ok = (fscanf(f, "%d", &v) == 1);
	fclose(f);
	if (ok && out_val)
		*out_val = v;
	return ok;
}

/*
 * Check whether kernel CPU `hwid` is online via /sys/devices/system/cpu/cpuN/online.
 * Note: cpu0 on many x86 Linux kernels has no `online` file when non-hotpluggable,
 * which means it is permanently online.
 */
static bool is_cpu_online_sysfs(int hwid)
{
	char path[256];
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/online", hwid);
	int online = 1;
	if (!read_sysfs_int(path, &online))
		return true;
	return online != 0;
}

static int parse_cpulist(const char *str, int *cpus, int max_cpus)
{
	int count = 0;
	if (!str || *str == '\0')
		return 0;

	char buf[4096];
	strncpy(buf, str, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *saveptr = NULL;
	char *token = strtok_r(buf, ",", &saveptr);
	while (token && count < max_cpus) {
		while (*token && isspace((unsigned char)*token))
			token++;
		if (*token == '\0') {
			token = strtok_r(NULL, ",", &saveptr);
			continue;
		}
		int start = 0, end = 0;
		if (sscanf(token, "%d-%d", &start, &end) == 2) {
			for (int i = start; i <= end && count < max_cpus; i++) {
				cpus[count++] = i;
			}
		} else if (sscanf(token, "%d", &start) == 1) {
			cpus[count++] = start;
		}
		token = strtok_r(NULL, ",", &saveptr);
	}
	return count;
}

static void read_sysfs_topology_fallback(int hwid, int *socket_id, int *core_id,
					 int *smt_id)
{
	char path[256];
	int pkg = 0, cid = hwid;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
		 hwid);
	if (read_sysfs_int(path, &pkg) && pkg >= 0)
		*socket_id = pkg;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/topology/core_id", hwid);
	if (read_sysfs_int(path, &cid) && cid >= 0)
		*core_id = cid;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list",
		 hwid);
	FILE *f = fopen(path, "r");
	if (f) {
		char line[256];
		if (fgets(line, sizeof(line), f)) {
			int sibs[64];
			int nsibs = parse_cpulist(line, sibs, 64);
			*smt_id = 0;
			for (int i = 0; i < nsibs; i++) {
				if (sibs[i] == hwid) {
					*smt_id = i;
					break;
				}
			}
		}
		fclose(f);
	}
}

/*
 * Probe CPU topology on the currently pinned logical CPU via x86 CPUID:
 *  1) Primary: CPUID leaf 0x0B subleaf 0 (SMT) & subleaf 1 (Core/Package),
 *     matching intel/mlc FUN_00126980.
 *  2) Fallback for AMD without usable 0x0B: CPUID leaf 0x8000001E / 0x80000008
 *     (matching FUN_00126750).
 *  3) Fallback for legacy Intel: CPUID leaf 0x04 + leaf 0x01 initial APIC ID
 *     (matching FUN_00126540), with sysfs topology fallback for masked VMs.
 */
static void probe_current_cpu_cpuid(int hwid, cpu_topology_entry_t *entry)
{
	unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
	unsigned int max_std_leaf = 0;
	char vendor[13] = { 0 };

	if (__get_cpuid(0, &eax, &ebx, &ecx, &edx)) {
		max_std_leaf = eax;
		memcpy(vendor, &ebx, 4);
		memcpy(vendor + 4, &edx, 4);
		memcpy(vendor + 8, &ecx, 4);
		vendor[12] = '\0';
	}

	entry->smt_id = 0;
	entry->core_id = hwid;
	entry->socket_id = 0;
	entry->core_type = 0;

	bool resolved = false;

	if (max_std_leaf >= 0x0b) {
		unsigned int eax0 = 0, ebx0 = 0, ecx0 = 0, edx0 = 0;
		__cpuid_count(0x0b, 0, eax0, ebx0, ecx0, edx0);
		if (eax0 != 0 || ebx0 != 0) {
			unsigned int eax1 = 0, ebx1 = 0, ecx1 = 0, edx1 = 0;
			__cpuid_count(0x0b, 1, eax1, ebx1, ecx1, edx1);

			uint32_t smt_shift = eax0 & 0x1fU;
			uint32_t pkg_shift = eax1 & 0x1fU;
			if (pkg_shift < smt_shift)
				pkg_shift = smt_shift;

			uint32_t x2apic_id = edx0;
			uint32_t smt_mask =
			    (smt_shift >= 32) ? 0xffffffffU
					      : ((1U << smt_shift) - 1U);
			uint32_t pkg_mask =
			    (pkg_shift >= 32) ? 0U
					      : (~((1U << pkg_shift) - 1U));
			uint32_t core_mask = (~smt_mask) ^ pkg_mask;

			entry->smt_id = (int)(x2apic_id & smt_mask);
			entry->core_id =
			    (smt_shift >= 32) ? 0
					      : (int)((x2apic_id & core_mask) >>
						      smt_shift);
			entry->socket_id =
			    (pkg_shift >= 32) ? 0
					      : (int)((x2apic_id & pkg_mask) >>
						      pkg_shift);
			resolved = true;
		}
	}

	if (!resolved && strcmp(vendor, "AuthenticAMD") == 0) {
		unsigned int ext_max = 0, b = 0, c = 0, d = 0;
		__cpuid(0x80000000U, ext_max, b, c, d);
		if (ext_max >= 0x8000001eU) {
			unsigned int apic_id = 0, ebx_1e = 0, ecx_1e = 0, edx_1e = 0;
			__cpuid(0x8000001eU, apic_id, ebx_1e, ecx_1e, edx_1e);
			uint32_t threads_per_core = ((ebx_1e >> 8) & 0xffU) + 1U;
			uint32_t core_unit_id = ebx_1e & 0xffU;

			uint32_t pkg_shift = 0;
			if (ext_max >= 0x80000008U) {
				unsigned int a8 = 0, b8 = 0, c8 = 0, d8 = 0;
				__cpuid(0x80000008U, a8, b8, c8, d8);
				pkg_shift = (c8 >> 12) & 0xfU;
				if (pkg_shift == 0)
					pkg_shift = ceil_log2_u32((c8 & 0xffU) + 1U);
			}

			entry->smt_id = (threads_per_core > 1)
			    ? (int)(apic_id % threads_per_core)
			    : 0;
			entry->core_id = (int)core_unit_id;
			entry->socket_id = (pkg_shift > 0 && pkg_shift < 32)
			    ? (int)(apic_id >> pkg_shift)
			    : 0;
			resolved = true;
		}
	}

	if (!resolved && max_std_leaf >= 1) {
		unsigned int a1 = 0, b1 = 0, c1 = 0, d1 = 0;
		__cpuid(1, a1, b1, c1, d1);
		uint32_t apic_id = (b1 >> 24) & 0xffU;
		uint32_t max_logical = (b1 >> 16) & 0xffU;
		bool htt = ((d1 >> 28) & 1U) != 0;

		uint32_t max_cores = 1;
		if (max_std_leaf >= 4) {
			unsigned int a4 = 0, b4 = 0, c4 = 0, d4 = 0;
			__cpuid_count(4, 0, a4, b4, c4, d4);
			max_cores = ((a4 >> 26) & 0x3fU) + 1U;
		}

		if (htt && max_logical > 1) {
			uint32_t pkg_shift = ceil_log2_u32(max_logical);
			uint32_t threads_per_core =
			    (max_cores > 0 && max_logical >= max_cores)
			    ? (max_logical / max_cores)
			    : 1U;
			uint32_t smt_shift = ceil_log2_u32(threads_per_core);
			uint32_t smt_mask = (1U << smt_shift) - 1U;
			uint32_t pkg_mask = ~((1U << pkg_shift) - 1U);
			uint32_t core_mask = (~smt_mask) ^ pkg_mask;

			entry->smt_id = (int)(apic_id & smt_mask);
			entry->core_id = (int)((apic_id & core_mask) >> smt_shift);
			entry->socket_id = (int)((apic_id & pkg_mask) >> pkg_shift);
			resolved = true;
		}
	}

	if (!resolved) {
		read_sysfs_topology_fallback(hwid, &entry->socket_id,
					     &entry->core_id, &entry->smt_id);
	}

	if (max_std_leaf >= 0x1a) {
		unsigned int a1a = 0, b1a = 0, c1a = 0, d1a = 0;
		__cpuid_count(0x1a, 0, a1a, b1a, c1a, d1a);
		entry->core_type = (int)((a1a >> 24) & 0xffU);
	}
}

/*
 * Build per-socket CPU lists and reserved core pairs, matching FUN_00125fa0:
 *  - reserved_cpus[0]: keeps socket active (busythread)
 *  - reserved_cpus[1]: latency measurement / allocation homing core
 *  - If any active socket has < 2 physical cores (smt_id == 0), sets
 *    g_insufficient_cores_for_busy = true (DAT_0040d954 = 1).
 */
static void build_socket_tables(void)
{
	int socket_ids[MAX_SOCKETS];
	int num_sock = 0;

	for (int i = 0; i < g_total_cpus; i++) {
		int sid = g_cpus[i].socket_id;
		bool seen = false;
		for (int s = 0; s < num_sock; s++) {
			if (socket_ids[s] == sid) {
				seen = true;
				break;
			}
		}
		if (!seen && num_sock < MAX_SOCKETS)
			socket_ids[num_sock++] = sid;
	}

	qsort(socket_ids, (size_t)num_sock, sizeof(int), cmp_int_asc);
	g_num_sockets = num_sock;

	for (int s = 0; s < g_num_sockets; s++) {
		socket_info_t *sock = &g_sockets[s];
		memset(sock, 0, sizeof(*sock));
		sock->socket_id = socket_ids[s];
		sock->reserved_cpus[0] = -1;
		sock->reserved_cpus[1] = -1;

		for (int i = 0; i < g_total_cpus; i++) {
			if (g_cpus[i].socket_id != sock->socket_id)
				continue;
			if (sock->num_cpus < MAX_CPUS_PER_NODE)
				sock->cpus[sock->num_cpus++] = i;
			if (g_cpus[i].smt_id == 0 &&
			    sock->num_one_tpc_cpus < MAX_CPUS_PER_NODE) {
				sock->one_tpc_cpus[sock->num_one_tpc_cpus++] = i;
			}
		}

		/* If only non-zero SMT siblings are online, fall back to cpus[0] */
		if (sock->num_one_tpc_cpus == 0 && sock->num_cpus > 0) {
			sock->one_tpc_cpus[0] = sock->cpus[0];
			sock->num_one_tpc_cpus = 1;
		}

		if (sock->num_one_tpc_cpus >= 2) {
			sock->reserved_cpus[0] = sock->one_tpc_cpus[0];
			sock->reserved_cpus[1] = sock->one_tpc_cpus[1];
		} else if (sock->num_one_tpc_cpus == 1) {
			sock->reserved_cpus[0] = sock->one_tpc_cpus[0];
			sock->reserved_cpus[1] = sock->one_tpc_cpus[0];
			g_insufficient_cores_for_busy = true;
		}
	}
}

static void read_node_meminfo(int node_id, size_t *total_kb, size_t *free_kb)
{
	*total_kb = 0;
	*free_kb = 0;

	char path[256];
	snprintf(path, sizeof(path),
		 "/sys/devices/system/node/node%d/meminfo", node_id);
	FILE *f = fopen(path, "r");
	if (!f)
		return;

	char line[256];
	while (fgets(line, sizeof(line), f)) {
		size_t val = 0;
		if (strstr(line, "MemTotal:")) {
			if (sscanf(line, "%*s %*s %*s %zu", &val) == 1)
				*total_kb = val;
		} else if (strstr(line, "MemFree:")) {
			if (sscanf(line, "%*s %*s %*s %zu", &val) == 1)
				*free_kb = val;
		}
	}
	fclose(f);
}

/*
 * Discover sparse NUMA nodes via glob("/sys/devices/system/node/node[0-9].../meminfo")
 * + qsort, matching intel/mlc FUN_0010b3c0 + FUN_00127060 + FUN_001272f0.
 * Falls back to Socket view (g_numa_mode = false) when sysfs NUMA is absent.
 */
static void build_numa_tables(void)
{
	g_num_nodes = 0;
	g_numa_mode = false;

	glob_t globbuf;
	memset(&globbuf, 0, sizeof(globbuf));
	int node_ids[MAX_NUMA_NODES];
	int found_nodes = 0;

	const char *prefix = "/sys/devices/system/node/node";
	size_t prefix_len = strlen(prefix);

	if (glob("/sys/devices/system/node/node*/meminfo", 0, NULL, &globbuf) == 0) {
		for (size_t i = 0;
		     i < globbuf.gl_pathc && found_nodes < MAX_NUMA_NODES;
		     i++) {
			const char *p = globbuf.gl_pathv[i];
			if (strncmp(p, prefix, prefix_len) != 0)
				continue;
			const char *num_str = p + prefix_len;
			if (!isdigit((unsigned char)*num_str))
				continue;
			node_ids[found_nodes++] = atoi(num_str);
		}
	}
	globfree(&globbuf);

	if (found_nodes > 0) {
		qsort(node_ids, (size_t)found_nodes, sizeof(int), cmp_int_asc);
		g_numa_mode = true;

		for (int idx = 0; idx < found_nodes; idx++) {
			int nid = node_ids[idx];
			numa_node_info_t *node = &g_nodes[g_num_nodes];
			memset(node, 0, sizeof(*node));
			node->node_id = nid;
			node->socket_id = -1;
			node->reserved_cpus[0] = -1;
			node->reserved_cpus[1] = -1;

			read_node_meminfo(nid, &node->mem_total_kb,
					  &node->mem_free_kb);
			node->has_memory = (node->mem_free_kb > 0);

			char cpulist_path[256];
			snprintf(cpulist_path, sizeof(cpulist_path),
				 "/sys/devices/system/node/node%d/cpulist", nid);
			FILE *f = fopen(cpulist_path, "r");
			if (f) {
				char line[4096];
				if (fgets(line, sizeof(line), f)) {
					line[strcspn(line, "\r\n")] = '\0';
					int hwids[MAX_CPUS];
					int nhw =
					    parse_cpulist(line, hwids, MAX_CPUS);
					for (int k = 0; k < nhw; k++) {
						int hw = hwids[k];
						if (hw < 0 || hw >= g_conf_cpus)
							continue;
						int os_id = g_hwid_to_osid[hw];
						if (os_id < 0)
							continue;
						g_cpus[os_id].numa_id = nid;
						if (node->num_cpus <
						    MAX_CPUS_PER_NODE) {
							node->cpus[node->num_cpus++] =
							    os_id;
						}
						if (g_cpus[os_id].smt_id == 0 &&
						    node->num_one_tpc_cpus <
							MAX_CPUS_PER_NODE) {
							node->one_tpc_cpus
							    [node->num_one_tpc_cpus++] =
							    os_id;
						}
					}
				}
				fclose(f);
			}

			if (node->num_cpus > 0) {
				node->socket_id =
				    g_cpus[node->cpus[0]].socket_id;
				if (node->num_one_tpc_cpus == 0) {
					node->one_tpc_cpus[0] = node->cpus[0];
					node->num_one_tpc_cpus = 1;
				}
				if (node->num_one_tpc_cpus >= 2) {
					node->reserved_cpus[0] =
					    node->one_tpc_cpus[0];
					node->reserved_cpus[1] =
					    node->one_tpc_cpus[1];
				} else {
					node->reserved_cpus[0] =
					    node->one_tpc_cpus[0];
					node->reserved_cpus[1] =
					    node->one_tpc_cpus[0];
					g_insufficient_cores_for_busy = true;
				}
			}

			g_num_nodes++;
		}
	}

	/* Fallback when sysfs NUMA is unavailable: Socket view (DAT_0040d958 = 0) */
	if (g_num_nodes == 0) {
		g_numa_mode = false;
		if (g_num_sockets > 0) {
			for (int s = 0;
			     s < g_num_sockets && g_num_nodes < MAX_NUMA_NODES;
			     s++) {
				const socket_info_t *sock = &g_sockets[s];
				numa_node_info_t *node = &g_nodes[g_num_nodes++];
				memset(node, 0, sizeof(*node));
				node->node_id = sock->socket_id;
				node->socket_id = sock->socket_id;
				node->has_memory = true;
				node->num_cpus = sock->num_cpus;
				memcpy(node->cpus, sock->cpus,
				       (size_t)sock->num_cpus * sizeof(int));
				node->num_one_tpc_cpus = sock->num_one_tpc_cpus;
				memcpy(node->one_tpc_cpus, sock->one_tpc_cpus,
				       (size_t)sock->num_one_tpc_cpus *
					   sizeof(int));
				node->reserved_cpus[0] = sock->reserved_cpus[0];
				node->reserved_cpus[1] = sock->reserved_cpus[1];
				for (int i = 0; i < sock->num_cpus; i++) {
					g_cpus[sock->cpus[i]].numa_id =
					    sock->socket_id;
				}
			}
		} else {
			numa_node_info_t *node = &g_nodes[0];
			memset(node, 0, sizeof(*node));
			node->node_id = 0;
			node->socket_id = 0;
			node->has_memory = true;
			node->num_cpus = g_total_cpus;
			for (int i = 0;
			     i < g_total_cpus && i < MAX_CPUS_PER_NODE; i++) {
				node->cpus[i] = i;
				node->one_tpc_cpus[i] = i;
			}
			node->num_one_tpc_cpus = node->num_cpus;
			node->reserved_cpus[0] = (g_total_cpus > 0) ? 0 : -1;
			node->reserved_cpus[1] =
			    (g_total_cpus > 1) ? 1 : node->reserved_cpus[0];
			g_num_nodes = 1;
		}
	}
}

static void probe_cache_hierarchy(void)
{
	uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
	__cpuid(0, eax, ebx, ecx, edx);
	uint32_t max_basic = eax;
	g_is_amd = (ebx == 0x68747541U && edx == 0x69746e65U &&
		    ecx == 0x444d4163U);
	g_is_xeon_atom = false;

	if (!g_is_amd && max_basic >= 1) {
		uint32_t a1 = 0, b1 = 0, c1 = 0, d1 = 0;
		__cpuid(1, a1, b1, c1, d1);
		uint32_t base_fam = (a1 >> 8) & 0xfU;
		uint32_t fam = base_fam +
			       ((base_fam == 0xfU) ? ((a1 >> 20) & 0xffU) : 0U);
		uint32_t model = ((a1 >> 4) & 0xfU) | ((a1 >> 12) & 0xf0U);
		if (fam == 6 &&
		    (model == 0xafU || model == 0xb6U || model == 0xddU)) {
			g_is_xeon_atom = true;
		}
	}

	cpu_cache_info_t info;
	memset(&info, 0, sizeof(info));

	if (g_is_amd) {
		uint32_t ext_max = 0, b0 = 0, c0 = 0, d0 = 0;
		__cpuid(0x80000000U, ext_max, b0, c0, d0);
		bool has_l3 = false;
		if (ext_max >= 0x8000001dU) {
			uint32_t a1 = 0, b1 = 0, c1 = 0, d1 = 0;
			__cpuid(0x80000001U, a1, b1, c1, d1);
			if ((c1 & (1U << 22)) != 0) {
				for (uint32_t sub = 0; sub < 8; sub++) {
					uint32_t ca = 0, cb = 0, cc = 0, cd = 0;
					__cpuid_count(0x8000001dU, sub, ca, cb,
						      cc, cd);
					uint32_t ctype = ca & 0x1fU;
					if (ctype == 0)
						break;
					uint32_t clevel = (ca >> 5) & 0x7U;
					size_t line_sz = (cb & 0xfffU) + 1U;
					size_t parts =
					    ((cb >> 12) & 0x3ffU) + 1U;
					size_t ways =
					    ((cb >> 22) & 0x3ffU) + 1U;
					size_t sets = (size_t)cc + 1U;
					size_t sz =
					    ways * parts * line_sz * sets;
					if (clevel == 1 && ctype == 1) {
						info.l1d_type = (int)ctype;
						info.l1d_bytes = sz;
					} else if (clevel == 1 && ctype == 2) {
						info.l1i_type = (int)ctype;
						info.l1i_bytes = sz;
					} else if (clevel == 2) {
						info.l2_type = (int)ctype;
						info.l2_bytes = sz;
					} else if (clevel == 3) {
						info.l3_type = (int)ctype;
						info.l3_bytes = sz;
						has_l3 = true;
					}
				}
			}
		}
		if (!has_l3 && ext_max >= 0x80000006U) {
			uint32_t a5 = 0, b5 = 0, c5 = 0, d5 = 0;
			__cpuid(0x80000005U, a5, b5, c5, d5);
			if (info.l1d_bytes == 0) {
				info.l1d_type = 1;
				info.l1d_bytes = (size_t)(c5 >> 24) << 10;
			}
			if (info.l1i_bytes == 0) {
				info.l1i_type = 2;
				info.l1i_bytes = (size_t)(d5 >> 24) << 10;
			}
			uint32_t a6 = 0, b6 = 0, c6 = 0, d6 = 0;
			__cpuid(0x80000006U, a6, b6, c6, d6);
			if (info.l2_bytes == 0) {
				info.l2_type = 3;
				info.l2_bytes = (size_t)(c6 >> 16) << 10;
			}
			if (info.l3_bytes == 0) {
				info.l3_type = 3;
				info.l3_bytes = (size_t)(d6 >> 18) << 19;
			}
		}
	} else if (max_basic >= 4) {
		for (uint32_t sub = 0; sub < 16; sub++) {
			uint32_t ca = 0, cb = 0, cc = 0, cd = 0;
			__cpuid_count(4, sub, ca, cb, cc, cd);
			uint32_t ctype = ca & 0x1fU;
			if (ctype == 0)
				break;
			uint32_t clevel = (ca >> 5) & 0x7U;
			size_t line_sz = (cb & 0xfffU) + 1U;
			size_t parts = ((cb >> 12) & 0x3ffU) + 1U;
			size_t ways = ((cb >> 22) & 0x3ffU) + 1U;
			size_t sets = (size_t)cc + 1U;
			size_t sz = ways * parts * line_sz * sets;
			if (clevel == 1 && ctype == 1) {
				info.l1d_type = (int)ctype;
				info.l1d_bytes = sz;
			} else if (clevel == 1 && ctype == 2) {
				info.l1i_type = (int)ctype;
				info.l1i_bytes = sz;
			} else if (clevel == 2) {
				info.l2_type = (int)ctype;
				info.l2_bytes = sz;
			} else if (clevel == 3) {
				info.l3_type = (int)ctype;
				info.l3_bytes = sz;
			}
		}
	}

	/* Sysfs fallback when running under hypervisors that mask CPUID leaf 4 */
	if (info.l1d_bytes == 0 || info.l2_bytes == 0 || info.l3_bytes == 0) {
		for (int idx = 0; idx < 8; idx++) {
			char p_lvl[256], p_typ[256], p_sz[256];
			int lvl = 0;
			snprintf(p_lvl, sizeof(p_lvl),
				 "/sys/devices/system/cpu/cpu0/cache/index%d/level",
				 idx);
			if (!read_sysfs_int(p_lvl, &lvl))
				continue;
			snprintf(p_typ, sizeof(p_typ),
				 "/sys/devices/system/cpu/cpu0/cache/index%d/type",
				 idx);
			snprintf(p_sz, sizeof(p_sz),
				 "/sys/devices/system/cpu/cpu0/cache/index%d/size",
				 idx);
			char typ_buf[64] = { 0 }, sz_buf[64] = { 0 };
			FILE *ft = fopen(p_typ, "r");
			if (ft) {
				if (!fgets(typ_buf, sizeof(typ_buf), ft))
					typ_buf[0] = '\0';
				fclose(ft);
			}
			FILE *fs = fopen(p_sz, "r");
			if (fs) {
				if (!fgets(sz_buf, sizeof(sz_buf), fs))
					sz_buf[0] = '\0';
				fclose(fs);
			}
			char *endp = NULL;
			unsigned long val = strtoul(sz_buf, &endp, 10);
			if (val == 0)
				continue;
			size_t mult = 1;
			if (endp && (*endp == 'K' || *endp == 'k'))
				mult = 1024;
			else if (endp && (*endp == 'M' || *endp == 'm'))
				mult = 1024 * 1024;
			size_t bytes = (size_t)val * mult;
			if (lvl == 1 && strncmp(typ_buf, "Data", 4) == 0 &&
			    info.l1d_bytes == 0) {
				info.l1d_type = 1;
				info.l1d_bytes = bytes;
			} else if (lvl == 1 &&
				   strncmp(typ_buf, "Instruction", 11) == 0 &&
				   info.l1i_bytes == 0) {
				info.l1i_type = 2;
				info.l1i_bytes = bytes;
			} else if (lvl == 2 && info.l2_bytes == 0) {
				info.l2_type = 3;
				info.l2_bytes = bytes;
			} else if (lvl == 3 && info.l3_bytes == 0) {
				info.l3_type = 3;
				info.l3_bytes = bytes;
			}
		}
	}

	if (info.l1i_bytes > 0) {
		g_cache_info.l1i_type = info.l1i_type;
		g_cache_info.l1i_bytes = info.l1i_bytes;
	}
	if (info.l1d_bytes > 0) {
		g_cache_info.l1d_type = info.l1d_type;
		g_cache_info.l1d_bytes = info.l1d_bytes;
	}
	if (info.l2_bytes > 0) {
		g_cache_info.l2_type = info.l2_type;
		g_cache_info.l2_bytes = info.l2_bytes;
	}
	if (info.l3_bytes > 0) {
		g_cache_info.l3_type = info.l3_type;
		g_cache_info.l3_bytes = info.l3_bytes;
	}
}

void numa_init_topology_ex(bool count_only_online)
{
	probe_cache_hierarchy();

	long conf = sysconf(_SC_NPROCESSORS_CONF);
	long onln = sysconf(_SC_NPROCESSORS_ONLN);
	if (conf <= 0)
		conf = (onln > 0) ? onln : 1;
	if (onln <= 0)
		onln = conf;
	if (count_only_online && onln < conf)
		conf = onln;
	if (conf > MAX_CPUS)
		conf = MAX_CPUS;

	g_conf_cpus = (int)conf;
	g_total_cpus = 0;
	g_insufficient_cores_for_busy = false;
	for (int i = 0; i < MAX_CPUS; i++)
		g_hwid_to_osid[i] = -1;

	/* Save calling thread's affinity mask (matching FUN_00125cd0) */
	cpu_set_t saved_affinity;
	CPU_ZERO(&saved_affinity);
	bool have_saved =
	    (sched_getaffinity(0, sizeof(saved_affinity), &saved_affinity) == 0);

	for (int hwid = 0; hwid < g_conf_cpus; hwid++) {
		if (!is_cpu_online_sysfs(hwid))
			continue;

		cpu_set_t probe_set;
		CPU_ZERO(&probe_set);
		CPU_SET(hwid, &probe_set);
		if (sched_setaffinity(0, sizeof(probe_set), &probe_set) != 0) {
			/* Skip CPUs disallowed by container/cgroup cpuset */
			continue;
		}

		int os_id = g_total_cpus++;
		g_hwid_to_osid[hwid] = os_id;

		cpu_topology_entry_t *entry = &g_cpus[os_id];
		memset(entry, 0, sizeof(*entry));
		entry->os_id = os_id;
		entry->hwid = hwid;
		entry->numa_id = -1;

		probe_current_cpu_cpuid(hwid, entry);
	}

	if (have_saved) {
		sched_setaffinity(0, sizeof(saved_affinity), &saved_affinity);
	}

	/* Safety fallback if sched_setaffinity could not probe any CPU */
	if (g_total_cpus == 0) {
		int fallback_n = (onln > MAX_CPUS) ? MAX_CPUS : (int)onln;
		for (int i = 0; i < fallback_n; i++) {
			g_hwid_to_osid[i] = i;
			memset(&g_cpus[i], 0, sizeof(g_cpus[i]));
			g_cpus[i].os_id = i;
			g_cpus[i].hwid = i;
			g_cpus[i].numa_id = 0;
			read_sysfs_topology_fallback(i, &g_cpus[i].socket_id,
						     &g_cpus[i].core_id,
						     &g_cpus[i].smt_id);
		}
		g_total_cpus = fallback_n;
	}

	build_socket_tables();
	build_numa_tables();
	/* Build L3 cache cluster topology across all discovered NUMA nodes */
	g_num_l3_clusters = 0;
	g_l3_fallback_used = false;
	g_l3_sampled_active = false;
	g_num_sampled_l3 = 0;

	int cpu_l3_id[MAX_CPUS];
	for (int i = 0; i < g_total_cpus; i++) {
		cpu_l3_id[i] = -1;
		int hwid = g_cpus[i].hwid;
		char p_lvl[256], p_id[256];
		int lvl = 0, cid = -1;
		snprintf(p_lvl, sizeof(p_lvl),
			 "/sys/devices/system/cpu/cpu%d/cache/index3/level",
			 hwid);
		snprintf(p_id, sizeof(p_id),
			 "/sys/devices/system/cpu/cpu%d/cache/index3/id",
			 hwid);
		if (read_sysfs_int(p_lvl, &lvl) && lvl == 3 &&
		    read_sysfs_int(p_id, &cid) && cid >= 0) {
			cpu_l3_id[i] = cid;
		} else {
			for (int idx = 0; idx < 8; idx++) {
				snprintf(p_lvl, sizeof(p_lvl),
					 "/sys/devices/system/cpu/cpu%d/cache/index%d/level",
					 hwid, idx);
				if (!read_sysfs_int(p_lvl, &lvl) || lvl != 3)
					continue;
				snprintf(p_id, sizeof(p_id),
					 "/sys/devices/system/cpu/cpu%d/cache/index%d/id",
					 hwid, idx);
				if (read_sysfs_int(p_id, &cid) && cid >= 0) {
					cpu_l3_id[i] = cid;
					break;
				}
			}
		}
	}

	for (int n = 0; n < g_num_nodes; n++) {
		const numa_node_info_t *node = &g_nodes[n];
		if (node->num_cpus <= 0)
			continue;

		int distinct_ids[MAX_L3_CLUSTERS];
		int n_distinct = 0;
		for (int c = 0; c < node->num_cpus; c++) {
			int os_id = node->cpus[c];
			int cid = (os_id >= 0 && os_id < g_total_cpus)
				      ? cpu_l3_id[os_id]
				      : -1;
			if (cid < 0)
				continue;
			bool seen = false;
			for (int k = 0; k < n_distinct; k++) {
				if (distinct_ids[k] == cid) {
					seen = true;
					break;
				}
			}
			if (!seen && n_distinct < MAX_L3_CLUSTERS)
				distinct_ids[n_distinct++] = cid;
		}

		if (n_distinct > 0) {
			qsort(distinct_ids, (size_t)n_distinct, sizeof(int),
			      cmp_int_asc);
			for (int k = 0;
			     k < n_distinct && g_num_l3_clusters < MAX_L3_CLUSTERS;
			     k++) {
				int cid = distinct_ids[k];
				l3_cluster_info_t *cl =
				    &g_l3_clusters[g_num_l3_clusters];
				memset(cl, 0, sizeof(*cl));
				cl->cluster_id = g_num_l3_clusters;
				cl->numa_id = node->node_id;
				cl->socket_id = node->socket_id;
				cl->cache_id = cid;
				for (int c = 0; c < node->num_cpus; c++) {
					int os_id = node->cpus[c];
					if (os_id >= 0 && os_id < g_total_cpus &&
					    cpu_l3_id[os_id] == cid &&
					    cl->num_cpus < MAX_CPUS_PER_NODE) {
						cl->cpus[cl->num_cpus++] = os_id;
					}
				}
				if (cl->num_cpus > 0) {
					cl->socket_id =
					    g_cpus[cl->cpus[0]].socket_id;
					g_num_l3_clusters++;
				}
			}
		} else if (g_num_l3_clusters < MAX_L3_CLUSTERS) {
			g_l3_fallback_used = true;
			l3_cluster_info_t *cl =
			    &g_l3_clusters[g_num_l3_clusters];
			memset(cl, 0, sizeof(*cl));
			cl->cluster_id = g_num_l3_clusters;
			cl->numa_id = node->node_id;
			cl->socket_id = node->socket_id;
			cl->cache_id = -1;
			cl->num_cpus = node->num_cpus;
			memcpy(cl->cpus, node->cpus,
			       (size_t)node->num_cpus * sizeof(int));
			g_num_l3_clusters++;
		}
	}
}

void numa_init_topology(void)
{
	numa_init_topology_ex(false);
}

void numa_print_cpuid_topology(bool include_numa)
{
	for (int i = 0; i < g_total_cpus; i++) {
		const cpu_topology_entry_t *e = &g_cpus[i];
		if (include_numa) {
			printf
			    ("OS id:%4d Core id:%4d Socket id:%4d Hyperthread id:%4d numa:%4d\n",
			     e->os_id, e->core_id, e->socket_id, e->smt_id,
			     e->numa_id);
		} else {
			printf
			    ("OS id:%4d Core id:%4d Socket id:%4d Hyperthread id:%4d\n",
			     e->os_id, e->core_id, e->socket_id, e->smt_id);
		}
	}
}

bool numa_is_numa_mode(void)
{
	return g_numa_mode;
}

bool numa_has_insufficient_cores_for_busy(void)
{
	return g_insufficient_cores_for_busy;
}

int numa_get_num_nodes(void)
{
	return g_num_nodes;
}

const numa_node_info_t *numa_get_node(int idx)
{
	if (idx < 0 || idx >= g_num_nodes)
		return NULL;
	return &g_nodes[idx];
}

const numa_node_info_t *numa_get_node_by_id(int node_id)
{
	for (int i = 0; i < g_num_nodes; i++) {
		if (g_nodes[i].node_id == node_id)
			return &g_nodes[i];
	}
	return NULL;
}

const numa_node_info_t *numa_get_node_for_cpu(int cpu)
{
	if (cpu >= 0 && cpu < g_total_cpus && g_cpus[cpu].numa_id >= 0) {
		const numa_node_info_t *by_id =
		    numa_get_node_by_id(g_cpus[cpu].numa_id);
		if (by_id)
			return by_id;
	}
	for (int n = 0; n < g_num_nodes; n++) {
		for (int c = 0; c < g_nodes[n].num_cpus; c++) {
			if (g_nodes[n].cpus[c] == cpu)
				return &g_nodes[n];
		}
	}
	return NULL;
}

int numa_get_num_sockets(void)
{
	return g_num_sockets;
}

const socket_info_t *numa_get_socket(int idx)
{
	if (idx < 0 || idx >= g_num_sockets)
		return NULL;
	return &g_sockets[idx];
}

const socket_info_t *numa_get_socket_by_id(int socket_id)
{
	for (int i = 0; i < g_num_sockets; i++) {
		if (g_sockets[i].socket_id == socket_id)
			return &g_sockets[i];
	}
	return NULL;
}

int numa_get_total_cpus(void)
{
	return g_total_cpus;
}

const cpu_topology_entry_t *cpu_get_topology(int cpu)
{
	if (cpu < 0 || cpu >= g_total_cpus)
		return NULL;
	return &g_cpus[cpu];
}

int cpu_get_hwid(int cpu)
{
	if (cpu >= 0 && cpu < g_total_cpus)
		return g_cpus[cpu].hwid;
	return cpu;
}

int cpu_get_socket_id(int cpu)
{
	if (cpu >= 0 && cpu < g_total_cpus)
		return g_cpus[cpu].socket_id;
	return -1;
}

bool cpu_is_smt_primary(int cpu)
{
	if (cpu < 0 || cpu >= g_total_cpus)
		return false;
	return g_cpus[cpu].smt_id == 0;
}

bool cpu_is_same_core(int cpu_a, int cpu_b)
{
	if (cpu_a < 0 || cpu_a >= g_total_cpus || cpu_b < 0 ||
	    cpu_b >= g_total_cpus) {
		return cpu_a == cpu_b;
	}
	return (g_cpus[cpu_a].socket_id == g_cpus[cpu_b].socket_id) &&
	       (g_cpus[cpu_a].core_id == g_cpus[cpu_b].core_id);
}

const cpu_cache_info_t *cpu_get_cache_info(void)
{
	return &g_cache_info;
}

bool cpu_is_amd_processor(void)
{
	return g_is_amd;
}

bool cpu_is_xeon_atom_core(void)
{
	return g_is_xeon_atom;
}

int numa_pin_cpu(int cpu)
{
	int hwid = cpu_get_hwid(cpu);
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(hwid, &cpuset);
	return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t),
				      &cpuset);
}

static hugepage_type_t g_hugepage_mode = HUGEPAGE_NONE;

void numa_set_hugepage_mode(hugepage_type_t mode)
{
	g_hugepage_mode = mode;
}

hugepage_type_t numa_get_hugepage_mode(void)
{
	return g_hugepage_mode;
}

int numa_get_num_l3_clusters(void)
{
	return g_num_l3_clusters;
}

const l3_cluster_info_t *numa_get_l3_cluster(int idx)
{
	if (idx < 0 || idx >= g_num_l3_clusters)
		return NULL;
	return &g_l3_clusters[idx];
}

int numa_find_l3_cluster_for_cpu(int cpu)
{
	for (int i = 0; i < g_num_l3_clusters; i++) {
		for (int c = 0; c < g_l3_clusters[i].num_cpus; c++) {
			if (g_l3_clusters[i].cpus[c] == cpu)
				return i;
		}
	}
	return -1;
}

int numa_copy_l3_cluster_cpus(int idx, int *out_cpus, int max_cpus)
{
	if (idx < 0 || idx >= g_num_l3_clusters || !out_cpus || max_cpus <= 0)
		return 0;
	int cnt = g_l3_clusters[idx].num_cpus;
	if (cnt > max_cpus)
		cnt = max_cpus;
	memcpy(out_cpus, g_l3_clusters[idx].cpus, (size_t)cnt * sizeof(int));
	return cnt;
}

void numa_reset_l3_sampling(void)
{
	g_l3_sampled_active = false;
	g_num_sampled_l3 = 0;
}

int numa_sample_l3_domains(bool full_l3_matrix)
{
	numa_reset_l3_sampling();
	if (full_l3_matrix || g_num_l3_clusters <= 4)
		return g_num_l3_clusters;

	for (int n = 0; n < g_num_nodes; n++) {
		int nid = g_nodes[n].node_id;
		int first = -1, second = -1, last = -1;
		int count_on_node = 0;
		for (int c = 0; c < g_num_l3_clusters; c++) {
			if (g_l3_clusters[c].numa_id != nid)
				continue;
			if (first < 0)
				first = c;
			else if (second < 0)
				second = c;
			last = c;
			count_on_node++;
		}
		if (first >= 0 && g_num_sampled_l3 < MAX_L3_CLUSTERS)
			g_sampled_l3_ids[g_num_sampled_l3++] = first;
		if (g_num_nodes == 1 && count_on_node >= 3 && second >= 0 &&
		    second != last && g_num_sampled_l3 < MAX_L3_CLUSTERS) {
			g_sampled_l3_ids[g_num_sampled_l3++] = second;
		}
		if (last >= 0 && last != first &&
		    g_num_sampled_l3 < MAX_L3_CLUSTERS) {
			g_sampled_l3_ids[g_num_sampled_l3++] = last;
		}
	}

	if (g_num_sampled_l3 >= 2 && g_num_sampled_l3 < g_num_l3_clusters)
		g_l3_sampled_active = true;

	return numa_get_effective_l3_count();
}

int numa_get_effective_l3_count(void)
{
	return g_l3_sampled_active ? g_num_sampled_l3 : g_num_l3_clusters;
}

int numa_get_effective_l3_cluster_id(int eff_idx)
{
	if (g_l3_sampled_active) {
		if (eff_idx < 0 || eff_idx >= g_num_sampled_l3)
			return -1;
		return g_sampled_l3_ids[eff_idx];
	}
	if (eff_idx < 0 || eff_idx >= g_num_l3_clusters)
		return -1;
	return eff_idx;
}

void numa_print_l3_sampling_notice(void)
{
	if (!g_l3_sampled_active)
		return;
	printf("Sampled %d of %d L3 domains (", g_num_sampled_l3,
	       g_num_l3_clusters);
	int limit = (g_num_sampled_l3 > 6) ? 6 : g_num_sampled_l3;
	for (int i = 0; i < limit; i++) {
		int cid = g_sampled_l3_ids[i];
		const l3_cluster_info_t *cl = numa_get_l3_cluster(cid);
		int sock = cl ? cl->socket_id : 0;
		printf("%sL3_%d on socket %d", (i == 0) ? "" : ", ", cid, sock);
	}
	if (g_num_sampled_l3 > limit)
		printf(" ...");
	printf(").\n");
	(void)g_l3_fallback_used;
}

static size_t round_alloc_size_for_mode(size_t size, hugepage_type_t mode)
{
	if (size == 0)
		return 0;
	size_t align = 4096ULL;
	if (mode == HUGEPAGE_1GB)
		align = 1ULL << 30;
	else if (mode == HUGEPAGE_2MB)
		align = 2ULL << 20;
	return (size + align - 1ULL) & ~(align - 1ULL);
}

size_t numa_round_alloc_size(size_t size)
{
	return round_alloc_size_for_mode(size, g_hugepage_mode);
}

void *alloc_mem_onnode(size_t size, int node_id, hugepage_type_t hugepage)
{
	if (size == 0)
		return NULL;
	if (hugepage == HUGEPAGE_NONE && g_hugepage_mode != HUGEPAGE_NONE) {
		hugepage = g_hugepage_mode;
	}
	size_t alloc_sz = round_alloc_size_for_mode(size, hugepage);

	int flags = MAP_PRIVATE | MAP_ANONYMOUS;
	if (hugepage == HUGEPAGE_2MB) {
#ifdef MAP_HUGETLB
		flags |= MAP_HUGETLB;
#ifdef MAP_HUGE_2MB
		flags |= MAP_HUGE_2MB;
#endif
#endif
	} else if (hugepage == HUGEPAGE_1GB) {
#ifdef MAP_HUGETLB
		flags |= MAP_HUGETLB;
#ifdef MAP_HUGE_1GB
		flags |= MAP_HUGE_1GB;
#endif
#endif
	}

	void *ptr = mmap(NULL, alloc_sz, PROT_READ | PROT_WRITE, flags, -1, 0);
	if (ptr == MAP_FAILED && hugepage != HUGEPAGE_NONE) {
		/* Fall back to 2MB-aligned anonymous pages with MADV_HUGEPAGE for full THP collapse */
		flags = MAP_PRIVATE | MAP_ANONYMOUS;
		size_t thp_align = 2ULL << 20;
		size_t raw_sz = alloc_sz + thp_align;
		void *raw = mmap(NULL, raw_sz, PROT_READ | PROT_WRITE, flags, -1, 0);
		if (raw != MAP_FAILED) {
			uintptr_t raw_addr = (uintptr_t)raw;
			uintptr_t aligned_addr =
			    (raw_addr + thp_align - 1ULL) & ~(thp_align - 1ULL);
			size_t prefix = (size_t)(aligned_addr - raw_addr);
			size_t suffix = thp_align - prefix;
			if (prefix > 0)
				munmap((void *)raw_addr, prefix);
			if (suffix > 0)
				munmap((void *)(aligned_addr + alloc_sz), suffix);
			ptr = (void *)aligned_addr;
#ifdef MADV_HUGEPAGE
			madvise(ptr, alloc_sz, MADV_HUGEPAGE);
#endif
		}
	}

	if (ptr == MAP_FAILED) {
		return NULL;
	}

	if (g_numa_mode && node_id >= 0 && node_id < MAX_NUMA_NODE_ID) {
		unsigned long nodemask[MAX_NUMA_NODE_ID / 64] = { 0 };
		nodemask[node_id / 64] |= (1ULL << (node_id % 64));
		syscall(SYS_mbind, ptr, alloc_sz, MPOL_BIND, nodemask,
			(unsigned long)(node_id + 2), MPOL_MF_STRICT);
	}

	/* Touch all pages to pre-fault them onto target NUMA node / socket */
	volatile uint8_t *p = (volatile uint8_t *)ptr;
	for (size_t offset = 0; offset < alloc_sz; offset += 4096) {
		p[offset] = 0x5A;
	}

	return ptr;
}

void free_mem_onnode(void *ptr, size_t size)
{
	if (ptr && size > 0) {
		size_t alloc_sz = numa_round_alloc_size(size);
		if (alloc_sz < size)
			alloc_sz = (size + 4095ULL) & ~4095ULL;
		munmap(ptr, alloc_sz);
	}
}
