/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef RTE_LIBRTE_HOSTSHMEM /* hide it from coverage */

#include <stdint.h>
#include <unistd.h>
#include <inttypes.h> 
#include <sys/mman.h>
#include <sys/file.h>
#include <string.h>
#include <sys/queue.h>

#include <rte_log.h>
#include <rte_pci.h>
#include <rte_memory.h>
#include <rte_eal.h>
#include <rte_eal_memconfig.h>
#include <rte_string_fns.h>
#include <rte_errno.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_common.h>
#include <rte_hostshmem.h>

#include "eal_internal_cfg.h"
#include "eal_private.h"

#define PCI_VENDOR_ID_HOSTSHMEM 0x1Af4
#define PCI_DEVICE_ID_HOSTSHMEM 0x1110

#define HOSTSHMEM_MAGIC 0x0BADC0DE
#define HOSTSHMEM_MAX_PAGES (1 << 12)
#define HOSTSHMEM_RESOURCE_PATH "/sys/bus/pci/devices/%04x:%02x:%02x.%x/resource2"
#define HOSTSHMEM_CONFIG_PATH "/var/run/.%s_hostshmem_config"
#define HOSTSHMEM_METADATA_PATH "/var/run/.dpdk_hostshmem_metadata_%s"  
#define HOSTSHMEM_HP_METADATA_PATH "/var/run/.dpdk_hostshmem_hp_metadata_%s"  

#define PHYS 0x1
#define VIRT 0x2
#define IOREMAP 0x4
#define FULL (PHYS|VIRT|IOREMAP)

#define METADATA_SIZE_ALIGNED \
	(RTE_ALIGN_CEIL(sizeof(struct rte_hostshmem_metadata),pagesz))

#define CONTAINS(x,y)\
	(((y).addr_64 >= (x).addr_64) && ((y).addr_64 < (x).addr_64 + (x).len))

#define DIM(x) (sizeof(x)/sizeof(x[0]))

struct hostshmem_pci_device {
	char path[PATH_MAX];
	phys_addr_t ioremap_addr;
};

/* data type to store in config */
struct hostshmem_segment {
	struct rte_hostshmem_metadata_entry entry;
	uint64_t align;
	char path[PATH_MAX];
};
struct hostshmem_shared_config {
	struct hostshmem_segment segment[RTE_MAX_MEMSEG];
	uint32_t segment_idx;
	struct hostshmem_pci_device pci_devs[RTE_LIBRTE_HOSTSHMEM_MAX_PCI_DEVS];
	uint32_t pci_devs_idx;
};
static struct hostshmem_shared_config * hostshmem_config;
static int memseg_idx;
static int pagesz;

struct memseg_cache_entry {
	char filepath[PATH_MAX];
	uint64_t offset;
	uint64_t len;
};

struct hostshmemconfig {
	struct rte_hostshmem_metadata metadata;
	struct hostshmem_hugepage_metadata hp_metadata;
	struct memseg_cache_entry memseg_cache[HOSTSHMEM_MAX_PAGES];
		/**< account for multiple files per segment case */
	struct flock lock;
	struct flock hp_lock;
	rte_spinlock_t sl;
};

static struct hostshmemconfig * hostshmem_new_config;

/* Tailq heads to add rings to */
TAILQ_HEAD(rte_ring_list, rte_tailq_entry);

/* Tailq heads to add mempools to */
TAILQ_HEAD(rte_mempool_list, rte_tailq_entry);

/*
 * Utility functions
 

static int
is_hostshmem_device(struct rte_pci_device * dev)
{
	return (dev->id.vendor_id == PCI_VENDOR_ID_HOSTSHMEM
			&& dev->id.device_id == PCI_DEVICE_ID_HOSTSHMEM);
}
*/

static void *
map_metadata(int fd, uint64_t len)
{
	return mmap(NULL, len, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
}

static void
unmap_metadata(void * ptr, uint64_t len)
{
	munmap(ptr, len);
}
 /*
static int
has_hostshmem_metadata(int fd, uint64_t len)
{
	struct rte_hostshmem_metadata metadata;
	void * ptr;

	ptr = map_metadata(fd, len);

	if (ptr == MAP_FAILED)
		return -1;

	metadata = *(struct rte_hostshmem_metadata*) (ptr);

	unmap_metadata(ptr, len);

	return metadata.magic_number == HOSTSHMEM_MAGIC;
}
*/
static void
remove_segment(struct hostshmem_segment * ms, int len, int idx)
{
	int i;

	for (i = idx; i < len - 1; i++)
		memcpy(&ms[i], &ms[i+1], sizeof(struct hostshmem_segment));
	memset(&ms[len-1], 0, sizeof(struct hostshmem_segment));
}

static int
overlap(const struct rte_memzone * mz1, const struct rte_memzone * mz2)
{
	uint64_t start1, end1, start2, end2;
	uint64_t p_start1, p_end1, p_start2, p_end2;
	uint64_t i_start1, i_end1, i_start2, i_end2;
	int result = 0;

	/* gather virtual addresses */
	start1 = mz1->addr_64;
	end1 = mz1->addr_64 + mz1->len;
	start2 = mz2->addr_64;
	end2 = mz2->addr_64 + mz2->len;

	/* gather physical addresses */
	p_start1 = mz1->phys_addr;
	p_end1 = mz1->phys_addr + mz1->len;
	p_start2 = mz2->phys_addr;
	p_end2 = mz2->phys_addr + mz2->len;

	/* gather ioremap addresses */
	i_start1 = mz1->ioremap_addr;
	i_end1 = mz1->ioremap_addr + mz1->len;
	i_start2 = mz2->ioremap_addr;
	i_end2 = mz2->ioremap_addr + mz2->len;

	/* check for overlap in virtual addresses */
	if (start1 >= start2 && start1 < end2)
		result |= VIRT;
	if (start2 >= start1 && start2 < end1)
		result |= VIRT;

	/* check for overlap in physical addresses */
	if (p_start1 >= p_start2 && p_start1 < p_end2)
		result |= PHYS;
	if (p_start2 >= p_start1 && p_start2 < p_end1)
		result |= PHYS;

	/* check for overlap in ioremap addresses */
	if (i_start1 >= i_start2 && i_start1 < i_end2)
		result |= IOREMAP;
	if (i_start2 >= i_start1 && i_start2 < i_end1)
		result |= IOREMAP;

	return result;
}

static int
adjacent(const struct rte_memzone * mz1, const struct rte_memzone * mz2)
{
	uint64_t start1, end1, start2, end2;
	uint64_t p_start1, p_end1, p_start2, p_end2;
	uint64_t i_start1, i_end1, i_start2, i_end2;
	int result = 0;

	/* gather virtual addresses */
	start1 = mz1->addr_64;
	end1 = mz1->addr_64 + mz1->len;
	start2 = mz2->addr_64;
	end2 = mz2->addr_64 + mz2->len;

	/* gather physical addresses */
	p_start1 = mz1->phys_addr;
	p_end1 = mz1->phys_addr + mz1->len;
	p_start2 = mz2->phys_addr;
	p_end2 = mz2->phys_addr + mz2->len;

	/* gather ioremap addresses */
	i_start1 = mz1->ioremap_addr;
	i_end1 = mz1->ioremap_addr + mz1->len;
	i_start2 = mz2->ioremap_addr;
	i_end2 = mz2->ioremap_addr + mz2->len;

	/* check if segments are virtually adjacent */
	if (start1 == end2)
		result |= VIRT;
	if (start2 == end1)
		result |= VIRT;

	/* check if segments are physically adjacent */
	if (p_start1 == p_end2)
		result |= PHYS;
	if (p_start2 == p_end1)
		result |= PHYS;

	/* check if segments are ioremap-adjacent */
	if (i_start1 == i_end2)
		result |= IOREMAP;
	if (i_start2 == i_end1)
		result |= IOREMAP;

	return result;
}

static int
has_adjacent_segments(struct hostshmem_segment * ms, int len)
{
	int i, j, a;

	for (i = 0; i < len; i++)
		for (j = i + 1; j < len; j++) {
			a = adjacent(&ms[i].entry.mz, &ms[j].entry.mz);

			/* check if segments are adjacent virtually and/or physically but
			 * not ioremap (since that would indicate that they are from
			 * different PCI devices and thus don't need to be concatenated.
			 */
			if ((a & (VIRT|PHYS)) > 0 && (a & IOREMAP) == 0)
				return 1;
		}
	return 0;
}

static int
has_overlapping_segments(struct hostshmem_segment * ms, int len)
{
	int i, j;

	for (i = 0; i < len; i++)
		for (j = i + 1; j < len; j++)
			if (overlap(&ms[i].entry.mz, &ms[j].entry.mz))
				return 1;
	return 0;
}

static int
seg_compare(const void * a, const void * b)
{
	const struct hostshmem_segment * s1 = (const struct hostshmem_segment*) a;
	const struct hostshmem_segment * s2 = (const struct hostshmem_segment*) b;

	/* move unallocated zones to the end */
	if (s1->entry.mz.addr == NULL && s2->entry.mz.addr == NULL)
		return 0;
	if (s1->entry.mz.addr == 0)
		return 1;
	if (s2->entry.mz.addr == 0)
		return -1;

	return s1->entry.mz.phys_addr > s2->entry.mz.phys_addr;
}

#ifdef RTE_LIBRTE_HOSTSHMEM_DEBUG
static void
entry_dump(struct rte_hostshmem_metadata_entry *e)
{
	RTE_LOG(DEBUG, EAL, "\tvirt: %p-%p\n", e->mz.addr,
			RTE_PTR_ADD(e->mz.addr, e->mz.len));
	RTE_LOG(DEBUG, EAL, "\tphys: 0x%" PRIx64 "-0x%" PRIx64 "\n",
			e->mz.phys_addr,
			e->mz.phys_addr + e->mz.len);
	RTE_LOG(DEBUG, EAL, "\tio: 0x%" PRIx64 "-0x%" PRIx64 "\n",
			e->mz.ioremap_addr,
			e->mz.ioremap_addr + e->mz.len);
	RTE_LOG(DEBUG, EAL, "\tlen: 0x%" PRIx64 "\n", e->mz.len);
	RTE_LOG(DEBUG, EAL, "\toff: 0x%" PRIx64 "\n", e->offset);
}
#endif



/*
 * Actual useful code
 */

/* read through metadata mapped from the HOSTSHMEM device */
static int
read_metadata(char * path, int path_len, int fd, uint64_t flen)
{
	struct rte_hostshmem_metadata *metadata;
	//struct rte_hostshmem_metadata_entry * entry;
	//int idx, i;
	void * ptr;

	//metadata = malloc(sizeof(struct rte_hostshmem_metadata));
	//memset(metadata, 0, sizeof(struct rte_hostshmem_metadata));
	
	RTE_LOG(DEBUG, EAL, "HOSTSHMEM: reading metadata %s %d\n", path, path_len);
	ptr = map_metadata(fd, flen);

	RTE_LOG(DEBUG, EAL, "HOSTSHMEM: map metadata %p\n", ptr);
	if (ptr == MAP_FAILED)
	{
		RTE_LOG(DEBUG, EAL, "HOSTSHMEM: map metadata fail\n");
		return -1;
	}	

	metadata = (struct rte_hostshmem_metadata*) (ptr);
	memcpy(&hostshmem_new_config->metadata, metadata, sizeof(struct rte_hostshmem_metadata));
	RTE_LOG(DEBUG, EAL, "Parsing metadata for \"%s\"\n", metadata->name);

	unmap_metadata(ptr, flen);

	
#ifdef DONT_COMPILE
	idx = hostshmem_config->segment_idx;

	for (i = 0; i < RTE_LIBRTE_HOSTSHMEM_MAX_ENTRIES &&
		idx <= RTE_MAX_MEMSEG; i++) {

		if (idx == RTE_MAX_MEMSEG) {
			RTE_LOG(ERR, EAL, "Not enough memory segments!\n");
			return -1;
		}

		entry = &metadata.entry[i];

		/* stop on uninitialized memzone */
		if (entry->mz.len == 0)
			break;

		/* copy metadata entry */
		memcpy(&hostshmem_config->segment[idx].entry, entry,
				sizeof(struct rte_hostshmem_metadata_entry));

		/* copy path */
		snprintf(hostshmem_config->segment[idx].path, path_len, "%s", path);

		idx++;
	}
	hostshmem_config->segment_idx = idx;
#endif
	return 0;
}

/* read through metadata mapped from the HOSTSHMEM device */
static int
read_hp_metadata(char * path, int path_len, int fd, uint64_t flen)
{
	struct hostshmem_hugepage_metadata * hp_metadata;
	void * ptr;

	//hp_metadata = malloc(sizeof(struct hostshmem_hugepage_metadata));
	//memset(hp_metadata, 0, sizeof(struct hostshmem_hugepage_metadata));
	
	RTE_LOG(DEBUG, EAL, "HOSTSHMEM: reading metadata %s %d\n", path, path_len);
	ptr = map_metadata(fd, flen);

	RTE_LOG(DEBUG, EAL, "HOSTSHMEM: map hp metadata %p\n", ptr);
	if (ptr == MAP_FAILED)
	{
		RTE_LOG(DEBUG, EAL, "HOSTSHMEM: map hp metadata fail\n");
		return -1;
	}	

	hp_metadata = (struct hostshmem_hugepage_metadata*) (ptr);
	memcpy(&hostshmem_new_config->hp_metadata, hp_metadata, sizeof(struct hostshmem_hugepage_metadata));
	RTE_LOG(DEBUG, EAL, "Parsing metadata for \"%s\"\n", hp_metadata->name);	

	unmap_metadata(ptr, flen);

	return 0;
}


/* check through each segment and look for adjacent or overlapping ones. */
static int
cleanup_segments(struct hostshmem_segment * ms, int tbl_len)
{
	struct hostshmem_segment * s, * tmp;
	int i, j, concat, seg_adjacent, seg_overlapping;
	uint64_t start1, start2, end1, end2, p_start1, p_start2, i_start1, i_start2;

	qsort(ms, tbl_len, sizeof(struct hostshmem_segment),
				seg_compare);

	while (has_overlapping_segments(ms, tbl_len) ||
			has_adjacent_segments(ms, tbl_len)) {

		for (i = 0; i < tbl_len; i++) {
			s = &ms[i];

			concat = 0;

			for (j = i + 1; j < tbl_len; j++) {
				tmp = &ms[j];

				/* check if this segment is overlapping with existing segment,
				 * or is adjacent to existing segment */
				seg_overlapping = overlap(&s->entry.mz, &tmp->entry.mz);
				seg_adjacent = adjacent(&s->entry.mz, &tmp->entry.mz);

				/* check if segments fully overlap or are fully adjacent */
				if ((seg_adjacent == FULL) || (seg_overlapping == FULL)) {

#ifdef RTE_LIBRTE_HOSTSHMEM_DEBUG
					RTE_LOG(DEBUG, EAL, "Concatenating segments\n");
					RTE_LOG(DEBUG, EAL, "Segment %i:\n", i);
					entry_dump(&s->entry);
					RTE_LOG(DEBUG, EAL, "Segment %i:\n", j);
					entry_dump(&tmp->entry);
#endif

					start1 = s->entry.mz.addr_64;
					start2 = tmp->entry.mz.addr_64;
					p_start1 = s->entry.mz.phys_addr;
					p_start2 = tmp->entry.mz.phys_addr;
					i_start1 = s->entry.mz.ioremap_addr;
					i_start2 = tmp->entry.mz.ioremap_addr;
					end1 = s->entry.mz.addr_64 + s->entry.mz.len;
					end2 = tmp->entry.mz.addr_64 + tmp->entry.mz.len;

					/* settle for minimum start address and maximum length */
					s->entry.mz.addr_64 = RTE_MIN(start1, start2);
					s->entry.mz.phys_addr = RTE_MIN(p_start1, p_start2);
					s->entry.mz.ioremap_addr = RTE_MIN(i_start1, i_start2);
					s->entry.offset = RTE_MIN(s->entry.offset, tmp->entry.offset);
					s->entry.mz.len = RTE_MAX(end1, end2) - s->entry.mz.addr_64;
					concat = 1;

#ifdef RTE_LIBRTE_HOSTSHMEM_DEBUG
					RTE_LOG(DEBUG, EAL, "Resulting segment:\n");
					entry_dump(&s->entry);

#endif
				}
				/* if segments not fully overlap, we have an error condition.
				 * adjacent segments can coexist.
				 */
				else if (seg_overlapping > 0) {
					RTE_LOG(ERR, EAL, "Segments %i and %i overlap!\n", i, j);
#ifdef RTE_LIBRTE_HOSTSHMEM_DEBUG
					RTE_LOG(DEBUG, EAL, "Segment %i:\n", i);
					entry_dump(&s->entry);
					RTE_LOG(DEBUG, EAL, "Segment %i:\n", j);
					entry_dump(&tmp->entry);
#endif
					return -1;
				}
				if (concat)
					break;
			}
			/* if we concatenated, remove segment at j */
			if (concat) {
				remove_segment(ms, tbl_len, j);
				tbl_len--;
				break;
			}
		}
	}

	return tbl_len;
}


static int
create_new_config(void)
{
	char path[PATH_MAX];
	int fd;

	/* build hostshmem config file path */
	snprintf(path, sizeof(path), HOSTSHMEM_CONFIG_PATH,
			internal_config.hugefile_prefix);

	fd = open(path, O_CREAT | O_RDWR, 0600);

	if (fd < 0) {
		RTE_LOG(ERR, EAL, "Could not open %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* try ex-locking first - if the file is locked, we have a problem */
	if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
		RTE_LOG(ERR, EAL, "Locking %s failed: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}

	if (ftruncate(fd, sizeof(struct hostshmemconfig)) < 0) {
		RTE_LOG(ERR, EAL, "ftruncate failed: %s\n", strerror(errno));
		return -1;
	}

	hostshmem_new_config = mmap(NULL, sizeof(struct hostshmemconfig),
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (hostshmem_new_config == MAP_FAILED)
		return -1;

	memset(hostshmem_new_config, 0, sizeof(struct hostshmemconfig));

	/* change the exclusive lock we got earlier to a shared lock */
	if (flock(fd, LOCK_SH | LOCK_NB) == -1) {
		RTE_LOG(ERR, EAL, "Locking %s failed: %s \n", path, strerror(errno));
		return -1;
	}

	close(fd);

	return 0;
}

static int
open_new_config(void)
{
	char path[PATH_MAX];
	int fd;

	snprintf(path, sizeof(path), HOSTSHMEM_CONFIG_PATH,
			internal_config.hugefile_prefix);

	fd = open(path, O_RDONLY);


	if (fd < 0 && errno == ENOENT)
		return 0;

	else if (fd < 0) {
		RTE_LOG(ERR, EAL, "Could not open %s: %s\n",
				path, strerror(errno));
		return -1;
	}

	if (flock(fd, LOCK_EX | LOCK_NB) != -1) {


		if (unlink(path) < 0) {
			RTE_LOG(ERR, EAL, "Could not remove %s: %s\n", path,
					strerror(errno));
			return -1;
		}


		flock(fd, LOCK_UN);
		close(fd);

		return 0;
	}

	hostshmem_new_config = mmap(NULL, sizeof(struct hostshmemconfig),
			PROT_READ, MAP_SHARED, fd, 0);

	if (hostshmem_new_config == MAP_FAILED)
		return -1;


	if (flock(fd, LOCK_SH | LOCK_NB) == -1) {
		RTE_LOG(ERR, EAL, "Locking %s failed: %s \n", path, strerror(errno));
		return -1;
	}

	close(fd);

	return 0;
}

/*
static int
create_shared_config(void)
{
	char path[PATH_MAX];
	int fd;

	
	snprintf(path, sizeof(path), HOSTSHMEM_CONFIG_PATH,
			internal_config.hugefile_prefix);

	fd = open(path, O_CREAT | O_RDWR, 0600);

	if (fd < 0) {
		RTE_LOG(ERR, EAL, "Could not open %s: %s\n", path, strerror(errno));
		return -1;
	}

	
	if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
		RTE_LOG(ERR, EAL, "Locking %s failed: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}

	if (ftruncate(fd, sizeof(struct hostshmem_shared_config)) < 0) {
		RTE_LOG(ERR, EAL, "ftruncate failed: %s\n", strerror(errno));
		return -1;
	}

	hostshmem_config = mmap(NULL, sizeof(struct hostshmem_shared_config),
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (hostshmem_config == MAP_FAILED)
		return -1;

	memset(hostshmem_config, 0, sizeof(struct hostshmem_shared_config));

	
	if (flock(fd, LOCK_SH | LOCK_NB) == -1) {
		RTE_LOG(ERR, EAL, "Locking %s failed: %s \n", path, strerror(errno));
		return -1;
	}

	close(fd);

	return 0;
}
*/
/* open shared config file and, if present, map the config.
 * having no config file is not an error condition, as we later check if
 * hostshmem_config is NULL (if it is, that means nothing was mapped). 
static int
open_shared_config(void)
{
	char path[PATH_MAX];
	int fd;


	snprintf(path, sizeof(path), HOSTSHMEM_CONFIG_PATH,
			internal_config.hugefile_prefix);

	fd = open(path, O_RDONLY);


	if (fd < 0 && errno == ENOENT)
		return 0;

	else if (fd < 0) {
		RTE_LOG(ERR, EAL, "Could not open %s: %s\n",
				path, strerror(errno));
		return -1;
	}

	if (flock(fd, LOCK_EX | LOCK_NB) != -1) {


		if (unlink(path) < 0) {
			RTE_LOG(ERR, EAL, "Could not remove %s: %s\n", path,
					strerror(errno));
			return -1;
		}


		flock(fd, LOCK_UN);
		close(fd);

		return 0;
	}

	hostshmem_config = mmap(NULL, sizeof(struct hostshmem_shared_config),
			PROT_READ, MAP_SHARED, fd, 0);

	if (hostshmem_config == MAP_FAILED)
		return -1;


	if (flock(fd, LOCK_SH | LOCK_NB) == -1) {
		RTE_LOG(ERR, EAL, "Locking %s failed: %s \n", path, strerror(errno));
		return -1;
	}

	close(fd);

	return 0;
}
*/
//schoi5
static void
hostshmem_metadata_dump(FILE *f)
{
	unsigned i = 0;
	struct hostshmemconfig * config;
	struct rte_hostshmem_metadata_entry *entry;
	struct hostshmem_hugepage_entry *hp_entry;

	if (f == NULL)
	{
		return;
	}

	config = hostshmem_new_config;
	if (config == NULL) {
		RTE_LOG(ERR, EAL, "Cannot find HOSTSHMEM config !\n");
		return;
	}
	
	//rte_spinlock_lock(&config->sl);

	entry = &config->metadata.entry[0];
	hp_entry = &config->hp_metadata.hp_entries[0];
	if (entry == NULL)
	{
		RTE_LOG(ERR, EAL, "HOSTSHMEM entry == NULL !\n");
	}
	if (entry->mz.addr == NULL)
	{
		RTE_LOG(ERR, EAL, "HOSTSHMEM entry->mz.addr == NULL !\n");
	}

	RTE_LOG(ERR, EAL, "HOSTSHMEM with %lu entries!\n", RTE_DIM(config->metadata.entry));
	while (entry->mz.addr != NULL && i < RTE_DIM(config->metadata.entry)) {

		fprintf(f, "Entry %u: name:<%-20s>, phys:0x%-15lx, len:0x%-15lx, "
			"virt:%-15p, off:0x%-15lx\n",
			i,
			entry->mz.name,
			entry->mz.phys_addr,
			entry->mz.len,
			entry->mz.addr,
			entry->offset);
		i++;

		entry++;
	}

	i = 0;
	while (hp_entry->mz.addr != NULL && i < RTE_DIM(config->hp_metadata.hp_entries)) {

		fprintf(f, "HP Entry %u: name:<%-20s>, phys:0x%-15lx, len:0x%-15lx, "
			"virt:%-15p path: %s\n",
			i,
			hp_entry->mz.name,
			hp_entry->mz.phys_addr,
			hp_entry->mz.len,
			hp_entry->mz.addr,
			hp_entry->path);
		i++;

		hp_entry++;
	}	
	
	//rte_spinlock_unlock(&config->sl);
}


//schoi5
static int
get_metadata_and_config(void)
{
	/*
	open level metadata - hugepage and object mem segment
	- don't need to create a hostshmem config, just update rte_config and rte_hugepage_info
	mmap metadata file - and dump the info
	mmmap the mem segment.
	*/
	char path[PATH_MAX];
	char hp_path[PATH_MAX];
	int fd = 0, hp_fd=0;
	
	/* build hostshmem config file path */
	snprintf(path, PATH_MAX, HOSTSHMEM_METADATA_PATH,
			internal_config.hsm_metadata);	
	/* build hostshmem config file path */
	snprintf(hp_path, PATH_MAX, HOSTSHMEM_HP_METADATA_PATH,
			internal_config.hsm_metadata);	

	if (rte_eal_process_type() == RTE_PROC_SECONDARY) {
		if (open_new_config() < 0) {
			RTE_LOG(ERR, EAL, "Could not open HOSTSHMEM config!\n");
			return -1;
		}
	}
	else
	{
		if (hostshmem_new_config == NULL &&
				create_new_config() < 0) {
			RTE_LOG(ERR, EAL, "Could not create HOSTSHMEM the config!\n");
			close(fd);
			return -1;
		}	
		
		/* try to find metadata */
		RTE_LOG(ERR, EAL, "HOSTSHMEM metadata fd = %d!\n", fd);
		fd = open(path, O_RDWR);
		RTE_LOG(ERR, EAL, "HOSTSHMEM fd = %d!\n", fd);
		if (fd < 0) {
			RTE_LOG(ERR, EAL, "Could not open metadata at %s\n", path);
			return -1;
		}
		if (read_metadata(path, PATH_MAX, fd, sizeof(struct rte_hostshmem_metadata)) < 0) {
			RTE_LOG(ERR, EAL, "Could not open metadata at %s\n", path);
			close(fd);
			return -1;
		}	

		/* try to find hp metadata */
		RTE_LOG(ERR, EAL, "HOSTSHMEM hp metadata hp_fd = %d!\n", hp_fd);
		hp_fd = open(hp_path, O_RDWR);
		RTE_LOG(ERR, EAL, "HOSTSHMEM hp metadata hp_fd = %d!\n", hp_fd);
		if (hp_fd < 0) {
			RTE_LOG(ERR, EAL, "Could not open metadata at %s\n", hp_path);
			return -1;
		}
		if (read_hp_metadata(hp_path, PATH_MAX, hp_fd, sizeof(struct hostshmem_hugepage_metadata)) < 0) {
			RTE_LOG(ERR, EAL, "Could not open metadata at %s\n", path);
			close(fd);
			return -1;
		}
	}
	/* dump hostshmem config*/
	hostshmem_metadata_dump(stdout);

	return 0;
}

#ifdef DONT_COMPILE
/* get shared config file and, if present, map the config.
 * having no config file is not an error condition, as we later check if
 * hostshmem_config is NULL (if it is, that means nothing was mapped). */
static int
get_shared_config(void)
{
	char path[PATH_MAX];
	int fd;

	/* build hostshmem config file path */
	snprintf(path, sizeof(path), HOSTSHMEM_CONFIG_PATH,
			internal_config.hugefile_prefix);	
	
	fd = open(path, O_RDONLY);

	/* if the file doesn't exist, just return success */
	if (fd < 0 && errno == ENOENT)
		return 0;
	/* else we have an error condition */
	else if (fd < 0) {
		RTE_LOG(ERR, EAL, "Could not open %s: %s\n",
				path, strerror(errno));
		return -1;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
		RTE_LOG(ERR, EAL, "Locking %s failed: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}
	
	hostshmem_config = mmap(0, sizeof(struct hostshmem_shared_config),
							PROT_READ, MAP_SHARED, fd, 0);

	if (hostshmem_config == MAP_FAILED)
	{
		RTE_LOG(ERR, EAL, "Mapping %s failed: %s \n", path, strerror(errno));
		return -1;
	}

	/* place a shared lock on config file */
	if (flock(fd, LOCK_SH | LOCK_NB) == -1) {
		RTE_LOG(ERR, EAL, "Locking %s failed: %s \n", path, strerror(errno));
		return -1;
	}
	
	close(fd);

	RTE_LOG(DEBUG, EAL, "HOSTSHMEM: get config done\n");
	return 0;
}
#endif

/*
 * This function does the following:
 *
 * 1) Builds a table of hostshmem_segments with proper offset alignment
 * 2) Cleans up that table so that we don't have any overlapping or adjacent
 *    memory segments
 * 3) Creates memsegs from this table and maps them into memory.
 */
static inline int
map_all_segments(void)
{
	struct hostshmem_segment ms_tbl[RTE_MAX_MEMSEG];
	//struct hostshmem_pci_device * pci_dev;
	struct rte_mem_config * mcfg;
	struct hostshmem_segment * seg;
	int fd, fd_zero;
	unsigned i;
	//unsigned j;
	struct rte_memzone mz;
	struct rte_memseg ms;
	void * base_addr;
	//uint64_t align, len;
	//phys_addr_t ioremap_addr;

	//ioremap_addr = 0;

	memset(ms_tbl, 0, sizeof(ms_tbl));
	memset(&mz, 0, sizeof(struct rte_memzone));
	memset(&ms, 0, sizeof(struct rte_memseg));

	return 0;
	
	/* first, build a table of memsegs to map, to avoid failed mmaps due to
	 * overlaps
	 */
	RTE_LOG(DEBUG, EAL, "HOSTSHMEM: hostshmem_config->segment_idx: %d\n", hostshmem_config->segment_idx); 
	for (i = 0; i < hostshmem_config->segment_idx && i <= RTE_MAX_MEMSEG; i++) {
		if (i == RTE_MAX_MEMSEG) {
			RTE_LOG(ERR, EAL, "Too many segments requested!\n");
			return -1;
		}

		seg = &hostshmem_config->segment[i];

		/* copy segment to table */
		memcpy(&ms_tbl[i], seg, sizeof(struct hostshmem_segment));

#ifdef DONT_COMPILE		
		/* find ioremap addr */
		for (j = 0; j < DIM(hostshmem_config->pci_devs); j++) {
			pci_dev = &hostshmem_config->pci_devs[j];
			if (!strncmp(pci_dev->path, seg->path, sizeof(pci_dev->path))) {
				ioremap_addr = pci_dev->ioremap_addr;
				break;
			}
		}
		if (ioremap_addr == 0) {
			RTE_LOG(ERR, EAL, "Cannot find ioremap addr!\n");
			return -1;
		}

		/* work out alignments */
		align = seg->entry.mz.addr_64 -
				RTE_ALIGN_FLOOR(seg->entry.mz.addr_64, 0x1000);
		len = RTE_ALIGN_CEIL(seg->entry.mz.len + align, 0x1000);

		/* save original alignments */
		ms_tbl[i].align = align;

		/* create a memory zone */
		mz.addr_64 = seg->entry.mz.addr_64 - align;
		mz.len = len;
		mz.hugepage_sz = seg->entry.mz.hugepage_sz;
		mz.phys_addr = seg->entry.mz.phys_addr - align;

		/* find true physical address */
		mz.ioremap_addr = ioremap_addr + seg->entry.offset - align;

		ms_tbl[i].entry.offset = seg->entry.offset - align;
#endif

		/* create a memory zone */
		mz.addr_64 = seg->entry.mz.addr_64;
		mz.len = seg->entry.mz.len;
		mz.hugepage_sz = seg->entry.mz.hugepage_sz;
		mz.phys_addr = seg->entry.mz.phys_addr;

		/* find true physical address */
		mz.ioremap_addr = mz.phys_addr + seg->entry.offset; 

		ms_tbl[i].entry.offset = seg->entry.offset;

		memcpy(&ms_tbl[i].entry.mz, &mz, sizeof(struct rte_memzone));
	}

	/* clean up the segments */
	memseg_idx = cleanup_segments(ms_tbl, hostshmem_config->segment_idx);

	if (memseg_idx < 0)
		return -1;

	mcfg = rte_eal_get_configuration()->mem_config;

	fd_zero = open("/dev/zero", O_RDWR);

	if (fd_zero < 0) {
		RTE_LOG(ERR, EAL, "Cannot open /dev/zero: %s\n", strerror(errno));
		return -1;
	}

	/* create memsegs and put them into DPDK memory */
	for (i = 0; i < (unsigned) memseg_idx; i++) {

		seg = &ms_tbl[i];

		ms.addr_64 = seg->entry.mz.addr_64;
		ms.hugepage_sz = seg->entry.mz.hugepage_sz;
		ms.len = seg->entry.mz.len;
		ms.nchannel = rte_memory_get_nchannel();
		ms.nrank = rte_memory_get_nrank();
		ms.phys_addr = seg->entry.mz.phys_addr;
		ms.ioremap_addr = seg->entry.mz.ioremap_addr;
		ms.socket_id = seg->entry.mz.socket_id;

		base_addr = mmap(ms.addr, ms.len,
				PROT_READ | PROT_WRITE, MAP_PRIVATE, fd_zero, 0);

		if (base_addr == MAP_FAILED || base_addr != ms.addr) {
			RTE_LOG(ERR, EAL, "Cannot map /dev/zero!\n");
			return -1;
		}

		fd = open(seg->path, O_RDWR);

		if (fd < 0) {
			RTE_LOG(ERR, EAL, "Cannot open %s: %s\n", seg->path,
					strerror(errno));
			return -1;
		}

		munmap(ms.addr, ms.len);

		base_addr = mmap(ms.addr, ms.len,
				PROT_READ | PROT_WRITE, MAP_SHARED, fd,
				seg->entry.offset);


		if (base_addr == MAP_FAILED || base_addr != ms.addr) {
			RTE_LOG(ERR, EAL, "Cannot map segment into memory: "
					"expected %p got %p (%s)\n", ms.addr, base_addr,
					strerror(errno));
			return -1;
		}

		RTE_LOG(DEBUG, EAL, "Memory segment mapped: %p (len %" PRIx64 ") at "
				"offset 0x%" PRIx64 "\n",
				ms.addr, ms.len, seg->entry.offset);

		/* put the pointers back into their real positions using original
		 * alignment */
		ms.addr_64 += seg->align;
		ms.phys_addr += seg->align;
		ms.ioremap_addr += seg->align;
		ms.len -= seg->align;

		/* at this point, the rest of DPDK memory is not initialized, so we
		 * expect memsegs to be empty */
		memcpy(&mcfg->memseg[i], &ms,
				sizeof(struct rte_memseg));

		close(fd);

		RTE_LOG(DEBUG, EAL, "HOSTSHMEM segment found, size: 0x%lx\n",
				ms.len);
	}

	return 0;
}

static inline int
map_hp_entries(void)
{
	struct hostshmem_hugepage_entry * hp_entry;
	struct rte_mem_config * mcfg;
	int fd, fd_zero;
	unsigned i = 0;
	struct rte_memseg ms;
	void * base_addr;
	
	if (hostshmem_new_config == NULL)
	{
		RTE_LOG(ERR, EAL, "hostshmem_new_config == NULL\n");
		return -1;
	}
	
	mcfg = rte_eal_get_configuration()->mem_config;

	fd_zero = open("/dev/zero", O_RDWR);

	if (fd_zero < 0) {
		RTE_LOG(ERR, EAL, "Cannot open /dev/zero: %s\n", strerror(errno));
		return -1;
	}

	hp_entry = &hostshmem_new_config->hp_metadata.hp_entries[0];
	
	/* create memsegs and put them into DPDK memory */
	while (hp_entry->mz.addr != NULL && i < RTE_DIM(hostshmem_new_config->hp_metadata.hp_entries)) {	

		ms.addr_64 = hp_entry->mz.addr_64;
		ms.hugepage_sz = hp_entry->mz.hugepage_sz;
		ms.len = hp_entry->mz.len;
		ms.nchannel = rte_memory_get_nchannel();
		ms.nrank = rte_memory_get_nrank();
		ms.phys_addr = hp_entry->mz.phys_addr;
		ms.ioremap_addr = hp_entry->mz.phys_addr;
		ms.socket_id = hp_entry->mz.socket_id;

		base_addr = mmap(ms.addr, ms.len,
				PROT_READ | PROT_WRITE, MAP_PRIVATE, fd_zero, 0);

		if (base_addr == MAP_FAILED || base_addr != ms.addr) {
			RTE_LOG(ERR, EAL, "Cannot map /dev/zero!\n");
			return -1;
		}

		fd = open(hp_entry->path, O_RDWR);

		if (fd < 0) {
			RTE_LOG(ERR, EAL, "Cannot open %s: %s\n", hp_entry->path,
					strerror(errno));
			return -1;
		}

		munmap(ms.addr, ms.len);

		base_addr = mmap(ms.addr, ms.len,
				PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

		if (base_addr == MAP_FAILED || base_addr != ms.addr) {
			RTE_LOG(ERR, EAL, "Cannot map segment into memory: "
					"expected %p got %p (%s)\n", ms.addr, base_addr,
					strerror(errno));
			return -1;
		}

		RTE_LOG(DEBUG, EAL, "Memory segment mapped: %p (len %" PRIx64 ") at "
				"\n",
				ms.addr, ms.len);

		/* at this point, the rest of DPDK memory is not initialized, so we
		 * expect memsegs to be empty */
		memcpy(&mcfg->memseg[i], &ms,
				sizeof(struct rte_memseg));

		close(fd);

		RTE_LOG(DEBUG, EAL, "HOSTSHMEM segment found, size: 0x%lx\n",
				ms.len);
				
		i++;
		hp_entry++;
	}

	return 0;
}


/* this happens at a later stage, after general EAL memory initialization */
int
rte_eal_hostshmem_obj_init(void)
{
	struct rte_ring_list* ring_list = NULL;
	struct rte_mempool_list* mempool_list = NULL;
	struct rte_mem_config * mcfg;
	struct rte_hostshmem_metadata_entry *entry;
	struct rte_memzone * mz;
	struct rte_mempool * mp;
	struct rte_ring * r;
	struct rte_tailq_entry *te;
	struct rte_tailq_entry *te1;
	unsigned i, idx;
	
	RTE_LOG(DEBUG, EAL, "HOSTSHMEM obj init!\n");
	/* secondary process would not need any object discovery - it'll all
	 * already be in shared config */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY || hostshmem_new_config == NULL)
	{
		RTE_LOG(DEBUG, EAL, "HOSTSHMEM Not Primary or Config NULL!\n");
		return 0;
	}
		
	RTE_LOG(DEBUG, EAL, "HOSTSHMEM init tailq!\n");
		/* check that we have an initialised mempool tail queue */
	mempool_list = RTE_TAILQ_LOOKUP("RTE_MEMPOOL", rte_mempool_list);
	if (mempool_list == NULL) {
		RTE_LOG(ERR, EAL, "No rte_mempool tailq found!\n");
		return -1;
	}
	
	/* check that we have an initialised ring tail queue */
	ring_list = RTE_TAILQ_LOOKUP(RTE_TAILQ_RING_NAME, rte_ring_list);
	if (ring_list == NULL) {
		RTE_LOG(ERR, EAL, "No rte_ring tailq found!\n");
		return -1;
	}

	mcfg = rte_eal_get_configuration()->mem_config;

	RTE_LOG(DEBUG, EAL, "HOSTSHMEM create memzone!\n");
	/* create memzones */
			
	entry = &hostshmem_new_config->metadata.entry[0];

	for (i = 0; i <  RTE_DIM(hostshmem_new_config->metadata.entry) && i <= RTE_MAX_MEMZONE; i++) {
		RTE_LOG(DEBUG, EAL, "looking at mem segment: %d\n",i);
		
		
		/* add memzone */
		if (mcfg->memzone_cnt == RTE_MAX_MEMZONE) {
			RTE_LOG(ERR, EAL, "No more memory zones available!\n");
			return -1;
		}

		idx = mcfg->memzone_cnt;

		RTE_LOG(DEBUG, EAL, "Found memzone: '%s' at %p (len 0x%" PRIx64 ")\n",
				entry->mz.name, entry->mz.addr, entry->mz.len);

		memcpy(&mcfg->memzone[idx], &entry->mz,
				sizeof(struct rte_memzone));

		/*  not needed find ioremap address 
		for (ms = 0; ms <= RTE_MAX_MEMSEG; ms++) {
			if (ms == RTE_MAX_MEMSEG) {
				RTE_LOG(ERR, EAL, "Physical address of segment not found!\n");
				return -1;
			}
			if (CONTAINS(mcfg->memseg[ms], mcfg->memzone[idx])) {
				offset = mcfg->memzone[idx].addr_64 -
								mcfg->memseg[ms].addr_64;
				mcfg->memzone[idx].ioremap_addr = mcfg->memseg[ms].ioremap_addr +
						offset;
				break;
			}
		}*/
		entry++;
		mcfg->memzone_cnt++;
	}

	rte_rwlock_write_lock(RTE_EAL_TAILQ_RWLOCK);
	RTE_LOG(DEBUG, EAL, "HOSTSHMEM finding mempool!\n");
	/* find mempool */
	for (i = 0; i < mcfg->memzone_cnt; i++) {
		mz = &mcfg->memzone[i];
 
		/* check if memzone has a ring prefix */
		if (strncmp(mz->name, RTE_MEMPOOL_MZ_PREFIX,
			sizeof(RTE_MEMPOOL_MZ_PREFIX) - 1) != 0)
				continue;
				
			mp = (struct rte_mempool*) (mz->addr_64);
			te1 = rte_zmalloc("MEMPOOL_TAILQ_ENTRY", sizeof(*te), 0);
			
		if (te1 == NULL) {
				RTE_LOG(ERR, EAL, "Cannot allocate mempool tailq entry!\n");
				return -1;
		}
			te1->data = (void *) mp;
			TAILQ_INSERT_TAIL(mempool_list, te1, next);
			RTE_LOG(DEBUG, EAL, "Found mempool: '%s' at %p\n", mp->name, mz->addr);
	}
	RTE_LOG(DEBUG, EAL, "HOSTSHMEM finding rings!\n");
	/* find rings */
	for (i = 0; i < mcfg->memzone_cnt; i++) {
		mz = &mcfg->memzone[i];

		/* check if memzone has a ring prefix */
		if (strncmp(mz->name, RTE_RING_MZ_PREFIX,
				sizeof(RTE_RING_MZ_PREFIX) - 1) != 0)
			continue;

		r = (struct rte_ring*) (mz->addr_64);
		/* schoi5 */
		r->memzone = mz;

		te = rte_zmalloc("RING_TAILQ_ENTRY", sizeof(*te), 0);
		if (te == NULL) {
			RTE_LOG(ERR, EAL, "Cannot allocate ring tailq entry!\n");
			return -1;
		}

		te->data = (void *) r;

		TAILQ_INSERT_TAIL(ring_list, te, next);

		RTE_LOG(DEBUG, EAL, "Found ring: '%s' at %p\n", r->name, mz->addr);
	}
	rte_rwlock_write_unlock(RTE_EAL_TAILQ_RWLOCK);

#ifdef RTE_LIBRTE_HOSTSHMEM_DEBUG
	RTE_LOG(DEBUG, EAL, "HOSTSHMEM Data Dump!\n");
	rte_memzone_dump(stdout);
	rte_ring_list_dump(stdout);
#endif

	return 0;
}

/* initialize hostshmem structures */
int rte_eal_hostshmem_init(void)
{
	char path[PATH_MAX];

	/* initialize everything to 0 */
	memset(path, 0, sizeof(path));
	hostshmem_config = NULL;

	pagesz = getpagesize();

	RTE_LOG(DEBUG, EAL, "Searching for HOSTSHMEM devices...\n");


	if (internal_config.hsm_metadata)
	{	
		RTE_LOG(ERR, EAL, "Open HOSTSHMEM metadata at %s\n", internal_config.hsm_metadata);
		if (get_metadata_and_config() < 0) {
			RTE_LOG(ERR, EAL, "Could not open HOSTSHMEM config!\n");
			return -1;
		}
	}
	/* hostshmem_config is not NULL only if config was created and/or mapped */
	if (hostshmem_config) {
		if (map_all_segments() < 0) {
			RTE_LOG(ERR, EAL, "Mapping HOSTSHMEM segments failed!\n");
			return -1;
		}
	}
	else if (hostshmem_new_config)
	{
		if (map_hp_entries() < 0) {
			RTE_LOG(ERR, EAL, "Mapping hp entries failed!\n");
			return -1;
		}
	}
	else {
		RTE_LOG(DEBUG, EAL, "No HOSTSHMEM configuration found! \n");
	}

	return 0;
}

#endif
