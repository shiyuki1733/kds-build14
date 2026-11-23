/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SDDC (slot deduplication / dictionary compression) for zram.
 */

#ifndef _ZRAM_SDDC_H_
#define _ZRAM_SDDC_H_

#include <linux/types.h>

struct page;
struct zs_pool;
struct zcomp;
struct zram_sddc;

/* Result of a store attempt. */
struct zram_sddc_store {
	void *meta;		/* NULL: plain slot, no SDDC bookkeeping */
	unsigned long handle;	/* zsmalloc handle (owned by the caller) */
	unsigned int size;	/* bytes charged to compr_data_size */
};

struct zram_sddc *zram_sddc_create(struct zs_pool *pool, struct zcomp *comp,
				   unsigned long huge_class_size);
void zram_sddc_destroy(struct zram_sddc *sddc);

int zram_sddc_store(struct zram_sddc *sddc, struct page *page,
		    struct zram_sddc_store *out);
int zram_sddc_load(struct zram_sddc *sddc, void *meta, struct page *page);
void zram_sddc_free_slot(struct zram_sddc *sddc, void *meta);
ssize_t zram_sddc_stat(struct zram_sddc *sddc, char *buf, size_t len);

int zram_sddc_mode_get(struct zram_sddc *sddc);
int zram_sddc_cand_get(struct zram_sddc *sddc);
void zram_sddc_cand_set(struct zram_sddc *sddc, int n);
void zram_sddc_mode_set(struct zram_sddc *sddc, int mode);
void zram_sddc_get_stats(struct zram_sddc *sddc, u64 *out);

#endif
