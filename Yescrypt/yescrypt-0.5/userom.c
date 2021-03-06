/*-
 * Copyright 2013,2014 Alexander Peslyak
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define YESCRYPT_FLAGS (YESCRYPT_RW | YESCRYPT_PWXFORM)
//#define YESCRYPT_FLAGS YESCRYPT_RW
//#define YESCRYPT_FLAGS YESCRYPT_WORM

#define YESCRYPT_MASK_SHM		1
#define YESCRYPT_MASK_FILE		0xe

#define ROM_SHM_KEY			0x524f4d0a

//#define DISABLE_ROM
//#define DUMP_LOCAL

#include <stdio.h>
#include <stdlib.h> /* for atoi() */
#include <string.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/times.h>

#ifdef _OPENMP
#include <omp.h>

#define NSAVE				1000
#endif

#include "yescrypt.h"

int main(int argc, const char * const *argv)
{
#if 0
	uint64_t rom_bytes = 112 * (1024ULL*1024*1024);
	uint64_t ram_bytes = 1 * (1024ULL*1024);
#else
	uint64_t rom_bytes = 3 * (1024ULL*1024*1024);
	uint64_t ram_bytes = 2 * (1024ULL*1024);
#endif
	uint32_t r, min_r;
	uint64_t NROM_log2, N_log2;
	yescrypt_shared_t shared;
#ifndef DISABLE_ROM
	int shmid;
#endif
	const char * rom_filename = NULL;
	int rom_fd;

	if (argc > 1)
		rom_bytes = atoi(argv[1]) * (1024ULL*1024*1024);
	if (argc > 2)
		ram_bytes = atoi(argv[2]) * (1024ULL*1024);
	if (argc > 3 && rom_bytes)
		rom_filename = argv[3];

	r = 8;
	min_r = 5;
	if (rom_filename)
		min_r = 8 * 64;

	NROM_log2 = 0;
	if (rom_bytes) {
		while (((rom_bytes >> NROM_log2) & 0xff) == 0)
			NROM_log2++;
		r = rom_bytes >> (7 + NROM_log2);
		while (r < min_r && NROM_log2 > 0) {
			r <<= 1;
			NROM_log2--;
		}
		rom_bytes = (uint64_t)r << (7 + NROM_log2);
	}

	N_log2 = 0;
	while (((uint64_t)r << (7 + N_log2)) < ram_bytes)
		N_log2++;
	ram_bytes = (uint64_t)r << (7 + N_log2);

	printf("r=%u N=2^%u NROM=2^%u\n", r,
	    (unsigned int)N_log2, (unsigned int)NROM_log2);

#ifdef DISABLE_ROM
	rom_bytes = 0;
#endif

	printf("Will use %.2f KiB ROM\n", rom_bytes / 1024.0);
	printf("         %.2f KiB RAM\n", ram_bytes / 1024.0);

	if (!rom_bytes && yescrypt_init_shared(&shared, NULL, 0, 0, 0, 0,
	    YESCRYPT_SHARED_DEFAULTS, 1, NULL, 0)) {
		puts("yescrypt_init_shared() FAILED");
		return 1;
	}

#ifndef DISABLE_ROM
	if (rom_filename) {
		rom_fd = open(rom_filename, O_RDONLY);
		if (rom_fd < 0) {
			perror("open");
			return 1;
		}

		int flags =
#ifdef MAP_NOCORE
		    MAP_NOCORE |
#endif
#ifdef MAP_HUGETLB
		    MAP_HUGETLB |
#endif
		    MAP_SHARED;
		void * p = mmap(NULL, rom_bytes, PROT_READ, flags, rom_fd, 0);
#ifdef MAP_HUGETLB
		if (p == MAP_FAILED)
			p = mmap(NULL, rom_bytes, PROT_READ,
			    flags & ~MAP_HUGETLB, rom_fd, 0);
#endif
		if (p == MAP_FAILED) {
			perror("mmap");
			close(rom_fd);
			return 1;
		}
		close(rom_fd);

		shared.shared1.base = shared.shared1.aligned = p;
		shared.shared1.aligned_size = rom_bytes;
		shared.mask1 = YESCRYPT_MASK_FILE;
	} else if (rom_bytes) {
		shared.shared1.aligned_size = rom_bytes;
		shmid = shmget(ROM_SHM_KEY, shared.shared1.aligned_size, 0);
		if (shmid == -1) {
			perror("shmget");
			return 1;
		}

		shared.shared1.base = shared.shared1.aligned =
		    shmat(shmid, NULL, SHM_RDONLY);
		if (shared.shared1.base == (void *)-1) {
			perror("shmat");
			return 1;
		}

		shared.mask1 = YESCRYPT_MASK_SHM;
	}
#endif

	if (rom_bytes)
		printf("ROM access frequency mask: 0x%x\n", shared.mask1);

	{
		yescrypt_local_t local;
		const uint8_t *setting;

		if (yescrypt_init_local(&local)) {
			puts("yescrypt_init_local() FAILED");
			return 1;
		}

		setting = yescrypt_gensalt(
		    N_log2, r, 1, YESCRYPT_FLAGS,
		    (const uint8_t *)"binary data", 12);

		{
			uint8_t hash[128];
			printf("'%s'\n", (char *)yescrypt_r(&shared, &local,
			    (const uint8_t *)"pleaseletmein", 13, setting,
			    hash, sizeof(hash)));
		}

#ifdef DUMP_LOCAL
#if 0
		fwrite(local.aligned, local.aligned_size, 1, stderr);
#else
		/* Skip B, dump only V */
		if (local.aligned_size >= ram_bytes + 128 * r)
			fwrite((char *)local.aligned + 128 * r, ram_bytes,
			    1, stderr);
#endif
#endif

		puts("Benchmarking 1 thread ...");

		clock_t clk_tck = sysconf(_SC_CLK_TCK);
		struct tms start_tms, end_tms;
		clock_t start = times(&start_tms), end;
		unsigned int i, n;
		unsigned long long count;
#ifdef _OPENMP
		char save[NSAVE][128];
		unsigned int nsave = 0;
#endif
		unsigned int seed = start * 1812433253U;

		n = 1;
		count = 0;
		do {
			for (i = 0; i < n; i++) {
				unsigned int j = count + i;
				char p[32];
				uint8_t hash[128];
				snprintf(p, sizeof(p), "%u", seed + j);
#ifdef _OPENMP
				const uint8_t *h =
#endif
				yescrypt_r(&shared,
				    &local,
				    (const uint8_t *)p, strlen(p),
				    setting, hash, sizeof(hash));
#ifdef _OPENMP
				if (j < NSAVE) {
					save[j][0] = 0;
					strncat(save[j], (char *)h,
					    sizeof(save[j]) - 1);
					nsave = j;
				}
#endif
			}
			count += n;

			end = times(&end_tms);
			n <<= 1;
		} while (end - start < clk_tck);

		clock_t start_v = start_tms.tms_utime + start_tms.tms_stime +
		    start_tms.tms_cutime + start_tms.tms_cstime;
		clock_t end_v = end_tms.tms_utime + end_tms.tms_stime +
		    end_tms.tms_cutime + end_tms.tms_cstime;

		printf("%llu c/s real, %llu c/s virtual "
		    "(%llu hashes in %.2f seconds)\n",
		    count * clk_tck / (end - start),
		    count * clk_tck / (end_v - start_v),
		    count, (double)(end - start) / clk_tck);

#ifdef _OPENMP
		unsigned int nt = omp_get_max_threads();

		printf("Benchmarking %u threads ...\n", nt);

		yescrypt_local_t locals[nt];

		unsigned int t;
		for (t = 0; t < nt; t++) {
			if (yescrypt_init_local(&locals[t])) {
				puts("yescrypt_init_local() FAILED");
				return 1;
			}
		}

		unsigned long long count1 = count, count_restart = 0;

		start = times(&start_tms);

		n = count;
		count = 0;
		do {
#pragma omp parallel for default(none) private(i) shared(n, shared, locals, setting, seed, count, save, nsave)
			for (i = 0; i < n; i++) {
				unsigned int j = count + i;
				char p[32];
				uint8_t hash[128];
				snprintf(p, sizeof(p), "%u", seed + j);
#if 1
				const uint8_t *h = yescrypt_r(&shared,
				    &locals[omp_get_thread_num()],
				    (const uint8_t *)p, strlen(p),
				    setting, hash, sizeof(hash));
#else
				yescrypt_local_t local;
				yescrypt_init_local(&local);
				const uint8_t *h = yescrypt_r(&shared,
				    &local,
				    (const uint8_t *)p, strlen(p),
				    setting, hash, sizeof(hash));
				yescrypt_free_local(&local);
#endif
				if (j < nsave && strcmp(save[j], (char *)h)) {
#pragma omp critical
					printf("Mismatch at %u, %s != %s\n",
					    j, save[j], (char *)h);
				}
			}

			count += n;
			if ((count - n) < count1 && count >= count1) {
				start = times(&start_tms);
				count_restart = count;
			} else
				n <<= 1;

			end = times(&end_tms);
		} while (end - start < clk_tck);

		if (!count_restart)
			puts("Didn't reach single-thread's hash count");
		count -= count_restart;

		start_v = start_tms.tms_utime + start_tms.tms_stime +
		    start_tms.tms_cutime + start_tms.tms_cstime;
		end_v = end_tms.tms_utime + end_tms.tms_stime +
		    end_tms.tms_cutime + end_tms.tms_cstime;

		printf("%llu c/s real, %llu c/s virtual "
		    "(%llu hashes in %.2f seconds)\n",
		    count * clk_tck / (end - start),
		    count * clk_tck / (end_v - start_v),
		    count, (double)(end - start) / clk_tck);
#endif
	}

	if (rom_filename && munmap(shared.shared1.base, rom_bytes)) {
		perror("munmap");
		return 1;
	}

	return 0;
}
