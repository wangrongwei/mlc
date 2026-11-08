#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fastdelay_dispatch.h"

static const int g_default_delays[DEFAULT_NUM_DELAYS] = {
	0, 2, 8, 15, 50, 100, 200, 300, 400, 500, 700, 1000, 1300, 1700, 2500,
	    3500, 5000, 9000, 20000
};

const int *get_default_delays(int *count)
{
	if (count)
		*count = DEFAULT_NUM_DELAYS;
	return g_default_delays;
}

int load_delays_from_file(const char *filename, int *out_delays, int max_delays)
{
	if (!filename || !out_delays || max_delays <= 0)
		return 0;

	FILE *f = fopen(filename, "r");
	if (!f)
		return -1;

	int count = 0;
	char line[128];
	while (fgets(line, sizeof(line), f) && count < max_delays) {
		int d = atoi(line);
		if (d >= 0) {
			out_delays[count++] = d;
		}
	}
	fclose(f);
	return count;
}

int parse_fastdelay_list(const char *delay_str, int *out_delays, int max_delays)
{
	if (!delay_str || !out_delays || max_delays <= 0)
		return 0;

	char buf[512];
	strncpy(buf, delay_str, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	int count = 0;
	char *saveptr = NULL;
	char *token = strtok_r(buf, ",", &saveptr);
	while (token && count < max_delays) {
		out_delays[count++] = atoi(token);
		token = strtok_r(NULL, ",", &saveptr);
	}
	return count;
}

#include <immintrin.h>

uint64_t run_fastdelay_kernel_worker(void *buf, size_t size, int traffic_type,
				     int delay_val, volatile bool *stop_flag)
{
	if (!buf || size < sizeof(__m256i) || !stop_flag)
		return 0;

	size_t count = size / sizeof(__m256i);
	__m256i *p = (__m256i *) buf;
	__m256i acc = _mm256_setzero_si256();
	__m256i val = _mm256_set1_epi64x(0x1234567890ABCDEFULL);
	uint64_t total_bytes = 0;

	while (!(*stop_flag)) {
		if (traffic_type == 6) {
			/* All Writes (W6) via non-temporal streaming stores */
			for (size_t i = 0; i < count; i += 4) {
				_mm256_stream_si256(&p[i], val);
				_mm256_stream_si256(&p[i + 1], val);
				_mm256_stream_si256(&p[i + 2], val);
				_mm256_stream_si256(&p[i + 3], val);

				if (delay_val > 0) {
					for (volatile int d = 0; d < delay_val;
					     d++) {
						_mm_pause();
					}
				}
			}
			_mm_sfence();
		} else {
			/* Read traffic (W1 All Reads, or mixed) */
			for (size_t i = 0; i < count; i += 4) {
				acc =
				    _mm256_add_epi64(acc,
						     _mm256_load_si256(&p[i]));
				acc =
				    _mm256_add_epi64(acc,
						     _mm256_load_si256(&p
								       [i +
									1]));
				acc =
				    _mm256_add_epi64(acc,
						     _mm256_load_si256(&p
								       [i +
									2]));
				acc =
				    _mm256_add_epi64(acc,
						     _mm256_load_si256(&p
								       [i +
									3]));

				if (delay_val > 0) {
					for (volatile int d = 0; d < delay_val;
					     d++) {
						_mm_pause();
					}
				}
			}
		}
		total_bytes += size;
	}

	int64_t dummy[4];
	_mm256_storeu_si256((__m256i *) dummy, acc);
	if (dummy[0] == 999999999LL)
		printf(" ");

	return total_bytes;
}
