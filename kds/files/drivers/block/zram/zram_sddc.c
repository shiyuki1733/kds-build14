// SPDX-License-Identifier: GPL-2.0
/*
 * SDDC: slot deduplication with dictionary (delta) compression for zram.
 *
 * Slot kinds on top of the ordinary compressed slot:
 *
 *   REF    a slot whose compressed stream stays addressable so later pages can
 *          be compressed against it.  It owns its zsmalloc handle.
 *   ALIAS  a slot whose compressed stream is byte identical to a REF; it stores
 *          no data at all and just points at the REF.
 *   DELTA  a slot stored as a dictionary coded stream relative to a REF.
 *
 * Lifetime: a REF is pinned by its dependents (meta->dependents).  When the
 * slot index of a REF is freed or overwritten the metadata survives as an
 * orphan and keeps the zsmalloc handle alive until the last dependent goes
 * away, so a dependent can never read a recycled reference.  The reference
 * graph is one level deep: only REF slots are referenced and REF slots have
 * no reference of their own.
 *
 * Locking: ->bucket_lock protects the candidate buckets and the
 * orphan/index transitions only; every pointer handed out is pinned with
 * ->dependents while the lock is held.  zsmalloc mapping and codec calls
 * always run outside that lock, so no sleeping operation happens under a
 * spinlock.
 *
 * All of this is a pure cache: any failure falls back to an ordinary
 * compressed slot.
 */

#include <linux/highmem.h>
#include <linux/jhash.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/zsmalloc.h>

#include "zcomp.h"
#include "zram_sddc.h"

#define SDDC_MAGIC		0x43444453	/* "SDDC" */
#define SDDC_VERSION		1
#define SDDC_HDR_BYTES		24
#define SDDC_INDEX_MIN_BYTES	256
#define SDDC_MAX_CANDIDATES	4
#define SDDC_BUCKET_BITS	14
#define SDDC_BUCKETS		(1U << SDDC_BUCKET_BITS)
#define SDDC_BUCKET_WAYS	6
#define SDDC_REF_CAP		32768
#define SDDC_DELTA_CAP		262144
#define SDDC_HASH_SEED		0x9e3779b9

/* LZ4KD needs >= 5 bytes of delta input and the wire must fit one page. */
#define SDDC_DELTA_LIMIT	(PAGE_SIZE - SDDC_HDR_BYTES)

/* Breadcrumbs: the last one printed before a hang names the failing step. */
#define SDDC_DBG	pr_warn
#define SDDC_MARK(_tag, _a, _b)	SDDC_DBG("KDSDBG %s %d %u\n", _tag, _a, _b)
#define SDDC_DELTA_MIN_BYTES	8

enum sddc_kind {
	SDDC_KIND_REF = 1,
	SDDC_KIND_ALIAS,
	SDDC_KIND_DELTA,
};

struct sddc_wire_hdr {
	u32 magic;
	u16 version;
	u16 kind;
	u32 ref_hash;
	u32 target_len;
	u32 payload_len;
	u32 payload_hash;
} __packed;

struct sddc_meta {
	struct zram_sddc *sddc;	/* owning manager, for deferred free */
	struct sddc_meta *ref;	/* reference slot, NULL for REF */
	unsigned long handle;	/* own stream (REF and DELTA only) */
	struct work_struct free_work;
	int dependents;
	int orphan;
	int kind;
	u32 gen;		/* generation, guards pointer reuse (ABA) */
	u32 stream_hash;	/* hash of own stream; for DELTA the ref's hash */
	u32 sample_hash;	/* head/tail page sample hash, bucket key */
	u16 wire_len;		/* bytes stored in zsmalloc */
	u16 target_len;
};

struct sddc_bucket {
	struct sddc_meta *slot[SDDC_BUCKET_WAYS];
};

struct zram_sddc {
	struct zs_pool *pool;
	struct zcomp *comp;
	unsigned long huge_class_size;

	struct sddc_bucket *buckets;
	spinlock_t bucket_lock;
	struct workqueue_struct *free_wq;
	atomic_t next_gen;

	/* encode scratch, serialised by ->enc_lock */
	void *enc_pair;		/* 2 contiguous pages: ref | target */
	void *enc_out;		/* 1 page: stream / delta wire */
	void *enc_plain;	/* 1 page: preserved plain stream */
	void *enc_tmp;		/* 1 page: candidate stream copy */
	void *enc_tmp2;		/* 1 page: second read-back for comparison */
	void *proof_pair;	/* 2 contiguous pages: round-trip proof */
	struct mutex enc_lock;

	/* decode scratch, serialised by ->dec_lock */
	void *dec_pair;		/* 2 contiguous pages: ref | target */
	void *dec_hdr;		/* 1 page: delta wire */
	void *dec_tmp;		/* 1 page: candidate stream copy */
	spinlock_t dec_lock;

	atomic64_t alias_hits;
	atomic64_t delta_hits;
	atomic64_t fallbacks;
	atomic64_t read_errors;
	atomic64_t proof_failures;
	atomic64_t re_pin, re_plain, re_hdr, re_refdec, re_ddec;

	atomic64_t store_total;
	atomic64_t cand_total;
	atomic64_t miss_small, miss_refdec, miss_delta_enc, miss_delta_size;
	atomic64_t miss_proof, miss_wire, miss_class;
	atomic64_t meta_alive;
	atomic64_t alive_ref, alive_alias, alive_delta;
	atomic64_t wq_pending, zs_fail;
	atomic64_t ref_cap_skip, delta_cap_skip;
	atomic64_t delta_tried, miss_nodelta;
	atomic64_t dry_tried, dry_ok, dry_refdec, dry_ddec, dry_rdiff;
	atomic64_t ref_verify_fail, ref_read_fail;
	atomic_t mode;		/* 0 off, 1 alias only, 2 alias+delta */
	bool fail_zero;		/* zero-fill instead of EIO on load failure */
	char comp_name[16];
	bool has_delta;

	/* Last load failure snapshot, exported via /sys/block/zramX/sddc_stat. */
	u32 dbg_stage;	/* 0 ok, 1 badhdr, 2 refdec, 3 ddec */
	u32 dbg_magic, dbg_kind, dbg_plen, dbg_wire;
	u32 dbg_ref_hash, dbg_hdr_ref, dbg_phash, dbg_calc_phash;
	int dbg_refdec_ret, dbg_ddec_ret;
	int dbg_store_refdec_ret;
	int dbg_verify_ret;
	u32 dbg_store_wire;
	u8 dbg_store_head[16];
	u32 last_slen, last_dlen, last_wire;
	u8 dbg_ref_head[16], dbg_tgt_head[16];
	atomic64_t load_entries;
	u8 dbg_pg_head[16], dbg_pg_off, dbg_st_off;
};

static u32 sddc_hash_bytes(const void *buf, unsigned int len)
{
	return jhash(buf, len, SDDC_HASH_SEED);
}

/*
 * Sample hash over the first and the last 16 bytes of the page.  Similar
 * pages share it, which is what the delta candidate search needs.
 */
static u32 sddc_sample_hash(const u8 *page)
{
	u32 head = jhash(page, 16, SDDC_HASH_SEED);
	u32 tail = jhash(page + PAGE_SIZE - 16, 16, SDDC_HASH_SEED);

	return head ^ rol32(tail, 13);
}

static struct sddc_bucket *sddc_bucket(struct zram_sddc *sddc, u32 hash)
{
	return &sddc->buckets[hash >> (32 - SDDC_BUCKET_BITS)];
}

static int sddc_zs_write(struct zram_sddc *sddc, const void *buf,
			 unsigned int len, unsigned long *handle)
{
	void *dst;

	*handle = zs_malloc(sddc->pool, len,
			    GFP_ATOMIC | __GFP_NOWARN |
			    __GFP_HIGHMEM | __GFP_MOVABLE | __GFP_CMA);
	if (IS_ERR((void *)*handle)) {
		atomic64_inc(&sddc->zs_fail);
		return -ENOMEM;
	}

	dst = zs_map_object(sddc->pool, *handle, ZS_MM_WO);
	memcpy(dst, buf, len);
	zs_unmap_object(sddc->pool, *handle);
	return 0;
}

static void sddc_read_stream(struct zram_sddc *sddc, unsigned long handle,
			     unsigned int len, void *buf)
{
	void *src;

	if (!handle)
		return;
	src = zs_map_object(sddc->pool, handle, ZS_MM_RO);
	memcpy(buf, src, len);
	zs_unmap_object(sddc->pool, handle);
}

/* Release the last pin: physical zsmalloc object and metadata go to the
 * free workqueue, outside any slot or bucket lock. */
static void sddc_free_workfn(struct work_struct *work)
{
	struct sddc_meta *meta = container_of(work, struct sddc_meta, free_work);
	struct zram_sddc *sddc = meta->sddc;

	if (meta->handle)
		zs_free(sddc->pool, meta->handle);
	if (meta->kind == SDDC_KIND_REF)
		atomic64_dec(&sddc->alive_ref);
	else if (meta->kind == SDDC_KIND_ALIAS)
		atomic64_dec(&sddc->alive_alias);
	else
		atomic64_dec(&sddc->alive_delta);
	atomic64_dec(&sddc->wq_pending);
	atomic64_dec(&sddc->meta_alive);
	kfree(meta);
}

/* Drop a pin; the last pin of an orphaned metadata frees it on the wq. */
static void sddc_put(struct zram_sddc *sddc, struct sddc_meta *meta)
{
	bool last;

	if (!meta)
		return;

	spin_lock(&sddc->bucket_lock);
	last = (--meta->dependents == 0) && meta->orphan;
	spin_unlock(&sddc->bucket_lock);

	if (last) {
		atomic64_inc(&sddc->wq_pending);
		queue_work(sddc->free_wq, &meta->free_work);
	}
}

static struct sddc_meta *sddc_meta_alloc(struct zram_sddc *sddc, int kind)
{
	struct sddc_meta *meta = kzalloc(sizeof(*meta), GFP_ATOMIC | __GFP_NOWARN);
	u32 gen;

	if (!meta)
		return NULL;

	gen = (u32)atomic_inc_return(&sddc->next_gen);
	if (!gen)
		gen = (u32)atomic_inc_return(&sddc->next_gen);

	meta->sddc = sddc;
	meta->kind = kind;
	meta->gen = gen;
	INIT_WORK(&meta->free_work, sddc_free_workfn);
	atomic64_inc(&sddc->meta_alive);
	if (kind == SDDC_KIND_REF)
		atomic64_inc(&sddc->alive_ref);
	else if (kind == SDDC_KIND_ALIAS)
		atomic64_inc(&sddc->alive_alias);
	else
		atomic64_inc(&sddc->alive_delta);
	return meta;
}

/* Pin a candidate while the bucket lock is held. */
static struct sddc_meta *sddc_get_locked(struct zram_sddc *sddc,
					 struct sddc_meta *meta)
{
	if (!meta || meta->kind != SDDC_KIND_REF || meta->orphan || !meta->gen)
		return NULL;
	meta->dependents++;
	return meta;
}

/* Transiently pin a reference for the duration of a read; validates kind and
 * generation under the bucket lock before handing it out.  An orphaned ref
 * (its own slot freed) is still kept alive by its dependents and stays valid. */
static struct sddc_meta *sddc_pin_ref(struct zram_sddc *sddc,
				      struct sddc_meta *ref)
{
	if (!ref)
		return NULL;

	spin_lock(&sddc->bucket_lock);
	if (ref->kind != SDDC_KIND_REF || !ref->gen) {
		spin_unlock(&sddc->bucket_lock);
		return NULL;
	}
	ref->dependents++;
	spin_unlock(&sddc->bucket_lock);
	return ref;
}

static void sddc_unindex(struct zram_sddc *sddc, struct sddc_meta *meta)
{
	struct sddc_bucket *b = sddc_bucket(sddc, meta->sample_hash);
	int i;

	for (i = 0; i < SDDC_BUCKET_WAYS; i++) {
		if (b->slot[i] == meta) {
			b->slot[i] = NULL;
			return;
		}
	}
}

static void sddc_index(struct zram_sddc *sddc, struct sddc_meta *meta)
{
	struct sddc_bucket *b = sddc_bucket(sddc, meta->sample_hash);
	int i, victim;

	for (i = 0; i < SDDC_BUCKET_WAYS; i++) {
		if (b->slot[i] == meta)
			return;
	}
	for (i = 0; i < SDDC_BUCKET_WAYS; i++) {
		if (!b->slot[i]) {
			b->slot[i] = meta;
			return;
		}
	}
	/* Bucket full: replace a random-ish entry, the old REF stays valid. */
	victim = sddc_hash_bytes(meta, sizeof(meta)) % SDDC_BUCKET_WAYS;
	b->slot[victim] = meta;
}

/*
 * Store @page.  Returns 0 with @out filled in; -ENOMEM when the allocation
 * itself failed; -EAGAIN when the caller must use the compression it already
 * performed elsewhere.
 */
static int sddc_store(struct zram_sddc *sddc, struct page *page,
		      struct zram_sddc_store *out);

int zram_sddc_store(struct zram_sddc *sddc, struct page *page,
		    struct zram_sddc_store *out)
{
	int ret;

	if (atomic_read(&sddc->mode) == 0)
		return -EAGAIN;

	mutex_lock(&sddc->enc_lock);
	ret = sddc_store(sddc, page, out);
	mutex_unlock(&sddc->enc_lock);
	return ret;
}

static int sddc_store(struct zram_sddc *sddc, struct page *page,
		      struct zram_sddc_store *out)
{
	struct zcomp_strm *zstrm;
	struct sddc_meta *cand[SDDC_MAX_CANDIDATES];
	struct sddc_meta *meta = NULL;
	struct sddc_bucket *bucket;
	const u8 *pagebuf;
	unsigned long handle = 0;
	unsigned int slen = 0, dlen = 0, wire_len;
	u32 sample, shash;
	int i, ncand = 0, ret = 0, sel = -1;
	static atomic_t dbg_cnt = ATOMIC_INIT(0);
	bool trace = atomic_inc_return(&dbg_cnt) <= 48;

	atomic64_inc(&sddc->store_total);
	pagebuf = kmap_atomic(page);
	memcpy(sddc->enc_pair + PAGE_SIZE, pagebuf, PAGE_SIZE);
	sample = sddc_sample_hash(pagebuf);
	kunmap_atomic((void *)pagebuf);

	/* 1. plain compression of the page */
	zstrm = zcomp_stream_get(sddc->comp);
	ret = zcomp_compress(zstrm, sddc->enc_pair + PAGE_SIZE, &slen);
	if (ret || slen == 0 || slen >= PAGE_SIZE ||
	    slen >= sddc->huge_class_size) {
		zcomp_stream_put(sddc->comp);
		/* Let the caller store the raw page, like the stock path. */
		return -EAGAIN;
	}
	memcpy(sddc->enc_out, zstrm->buffer, slen);
	memcpy(sddc->enc_plain, zstrm->buffer, slen);
	zcomp_stream_put(sddc->comp);
	sddc->last_slen = slen;

	shash = sddc_hash_bytes(sddc->enc_out, slen);

	/* 2. collect candidates, pinning them while the bucket lock is held */
	bucket = sddc_bucket(sddc, sample);
	spin_lock(&sddc->bucket_lock);
	for (i = 0; i < SDDC_BUCKET_WAYS && ncand < SDDC_MAX_CANDIDATES; i++) {
		struct sddc_meta *c = sddc_get_locked(sddc, bucket->slot[i]);

		if (c) {
			cand[ncand++] = c;
			atomic64_inc(&sddc->cand_total);
		}
	}
	spin_unlock(&sddc->bucket_lock);

	/* 3. exact stream match becomes an ALIAS */
	for (i = 0; i < ncand; i++) {
		if (cand[i]->wire_len != slen || cand[i]->stream_hash != shash)
			continue;
		sddc_read_stream(sddc, cand[i]->handle, slen, sddc->enc_tmp);
		if (memcmp(sddc->enc_tmp, sddc->enc_out, slen))
			continue;
		meta = sddc_meta_alloc(sddc, SDDC_KIND_ALIAS);
		if (!meta) {
			ret = -ENOMEM;
			goto drop;
		}
		meta->ref = cand[i];
		meta->wire_len = 0;
		meta->target_len = PAGE_SIZE;
		meta->stream_hash = shash;
		cand[i] = NULL;	/* the new slot takes over the pin */
		atomic64_inc(&sddc->alias_hits);
		out->meta = meta;
		out->handle = 0;
		out->size = 0;
		goto drop;
	}

	/* 4. dictionary compression against the first useful candidate */
	if (slen <= PAGE_SIZE / 8)
		atomic64_inc(&sddc->miss_small);
	if (atomic_read(&sddc->mode) >= 2 && slen > PAGE_SIZE / 8) {
		if (!sddc->has_delta) {
			atomic64_inc(&sddc->miss_nodelta);
		} else {
		for (i = 0; i < ncand; i++) {
			struct sddc_meta *c = cand[i];
			void *payload = sddc->enc_out + SDDC_HDR_BYTES;
			bool proof_bad = false;

			bool dry_run = (atomic_read(&sddc->mode) == 4);

			if (!c)
				continue;
			atomic64_inc(&sddc->delta_tried);
			if (dry_run)
				atomic64_inc(&sddc->dry_tried);

			if (trace)
				SDDC_MARK("d-read", i, c->wire_len);
			sddc_read_stream(sddc, c->handle, c->wire_len,
					 sddc->enc_tmp);
			if (trace)
				SDDC_MARK("d-rdok", i, 0);
			zstrm = zcomp_stream_get(sddc->comp);
			ret = zcomp_decompress(zstrm, sddc->enc_tmp,
					       c->wire_len, sddc->enc_pair);
			if (trace)
				SDDC_MARK("d-refdec", i, (unsigned)-ret);
			if (ret) {
				atomic64_inc(&sddc->miss_refdec);
				if (dry_run)
					atomic64_inc(&sddc->dry_refdec);
				sddc->dbg_store_refdec_ret = ret;
				sddc->dbg_store_wire = c->wire_len;
				memcpy(sddc->dbg_store_head, sddc->enc_tmp, 16);
				/* Same bytes, read a second time: if they differ the
				 * object moved under us; if they match the codec
				 * itself rejects the stream we stored. */
				sddc_read_stream(sddc, c->handle, c->wire_len,
						 sddc->enc_tmp2);
				if (memcmp(sddc->enc_tmp2, sddc->enc_tmp,
					   c->wire_len))
					atomic64_inc(&sddc->dry_rdiff);
			}
			if (!ret)
				ret = zcomp_compress_delta(zstrm,
						sddc->enc_pair,
						sddc->enc_pair + PAGE_SIZE,
						&dlen, SDDC_DELTA_LIMIT);
			if (trace)
				SDDC_MARK("d-enc", i, dlen);
			if (ret)
				atomic64_inc(&sddc->miss_delta_enc);
			if (!ret && (dlen < SDDC_DELTA_MIN_BYTES ||
				     dlen > SDDC_DELTA_LIMIT)) {
				ret = -E2BIG;
				atomic64_inc(&sddc->miss_delta_size);
			}
			if (!ret) {
				wire_len = SDDC_HDR_BYTES + dlen;
				if (wire_len + 8 >= slen) {
					ret = -E2BIG;
					atomic64_inc(&sddc->miss_wire);
				} else if (zs_lookup_class_index(wire_len) >=
					   zs_lookup_class_index(slen)) {
					ret = -E2BIG;
					atomic64_inc(&sddc->miss_class);
				}
			}
			if (!ret) {
				memcpy(payload, zstrm->buffer, dlen);
				if (trace)
					SDDC_MARK("d-copy", i, dlen);
				/*
				 * Round-trip proof, exactly like the reference
				 * implementation: never commit a delta that the
				 * codec cannot restore to the exact page.
				 */
				memcpy(sddc->proof_pair, sddc->enc_pair,
				       PAGE_SIZE);
				memset(sddc->proof_pair + PAGE_SIZE, 0xa5,
				       PAGE_SIZE);
				if (trace)
					SDDC_MARK("d-proof", i, dlen);
				ret = zcomp_decompress_delta(zstrm, payload, dlen,
						sddc->proof_pair,
						sddc->proof_pair + PAGE_SIZE);
				if (ret && dry_run)
					atomic64_inc(&sddc->dry_ddec);
				if (trace)
					SDDC_MARK("d-pdec", i, (unsigned)-ret);
				if (!ret &&
				    memcmp(sddc->proof_pair + PAGE_SIZE,
					   sddc->enc_pair + PAGE_SIZE,
					   PAGE_SIZE))
					proof_bad = true;
				if (trace)
					SDDC_MARK("d-pcmp", i, proof_bad);
			}
			zcomp_stream_put(sddc->comp);
			if (dry_run) {
				if (!ret && !proof_bad)
					atomic64_inc(&sddc->dry_ok);
				continue;
			}
			if (proof_bad) {
				atomic64_inc(&sddc->proof_failures);
				atomic64_inc(&sddc->miss_proof);
				pr_info_ratelimited("lz4kds: delta proof failed (%u bytes)\n",
						    dlen);
				continue;
			}
			if (ret)
				continue;

			sel = i;
			break;
		}

		if (sel >= 0) {
			struct sddc_wire_hdr *hdr = sddc->enc_out;
			struct sddc_meta *c = cand[sel];

			if (atomic64_read(&sddc->alive_delta) >=
			    SDDC_DELTA_CAP) {
				atomic64_inc(&sddc->delta_cap_skip);
				goto plain;
			}

			memset(hdr, 0, SDDC_HDR_BYTES);
			hdr->magic = SDDC_MAGIC;
			hdr->version = SDDC_VERSION;
			hdr->kind = SDDC_KIND_DELTA;
			hdr->ref_hash = c->stream_hash;
			hdr->target_len = PAGE_SIZE;
			hdr->payload_len = dlen;
			hdr->payload_hash = sddc_hash_bytes(sddc->enc_out +
					SDDC_HDR_BYTES, dlen);

			wire_len = SDDC_HDR_BYTES + dlen;
			sddc->last_dlen = dlen;
			sddc->last_wire = wire_len;
			if (sddc_zs_write(sddc, sddc->enc_out, wire_len,
					  &handle))
				goto plain;
			if (trace)
				SDDC_MARK("d-wrote", sel, wire_len);

			meta = sddc_meta_alloc(sddc, SDDC_KIND_DELTA);
			if (!meta) {
				zs_free(sddc->pool, handle);
				goto plain;
			}
			meta->ref = c;
			meta->handle = handle;
			meta->wire_len = wire_len;
			meta->target_len = PAGE_SIZE;
			meta->stream_hash = c->stream_hash;
			cand[sel] = NULL;	/* pin transferred to the new meta */
			atomic64_inc(&sddc->delta_hits);
			out->meta = meta;
			out->handle = handle;
			out->size = wire_len;
			goto drop;
		}
		}
	}
	(void)shash;

plain:
	if (sddc_zs_write(sddc, sddc->enc_plain, slen, &handle)) {
		ret = -ENOMEM;
		goto drop;
	}

	/* 5. promote the new slot to a reference when it is worth indexing.
	 * The stream must first survive a decode round-trip, otherwise a
	 * broken reference would poison every later delta that uses it. */
	if (slen >= SDDC_INDEX_MIN_BYTES && slen < sddc->huge_class_size &&
	    atomic64_read(&sddc->alive_ref) < SDDC_REF_CAP) {
		meta = sddc_meta_alloc(sddc, SDDC_KIND_REF);
		if (meta) {
			meta->handle = handle;
			meta->wire_len = slen;
			meta->target_len = PAGE_SIZE;
			meta->stream_hash = shash;
			meta->sample_hash = sample;
			spin_lock(&sddc->bucket_lock);
			sddc_index(sddc, meta);
			spin_unlock(&sddc->bucket_lock);
			out->meta = meta;
			out->handle = handle;
			out->size = slen;
			goto drop;
		}
		atomic64_inc(&sddc->fallbacks);
	} else if (slen >= SDDC_INDEX_MIN_BYTES &&
		   slen < sddc->huge_class_size) {
		atomic64_inc(&sddc->ref_cap_skip);
	}

	out->meta = NULL;
	out->handle = handle;
	out->size = slen;
	ret = 0;

drop:
	for (i = 0; i < ncand; i++)
		if (cand[i])
			sddc_put(sddc, cand[i]);
	return ret;
}

/* Rebuild a page out of an SDDC slot.  Runs under the zram slot spinlock. */
int zram_sddc_load(struct zram_sddc *sddc, void *meta_ptr, struct page *page)
{
	struct sddc_meta *meta = meta_ptr;
	struct sddc_meta *ref, *pinned = NULL;
	struct zcomp_strm *zstrm;
	struct sddc_wire_hdr *hdr;
	void *pagebuf;
	int ret = -EIO;
	static atomic_t dbg_cnt = ATOMIC_INIT(0);
	bool trace;

	if (!meta)
		return -EIO;

	spin_lock(&sddc->dec_lock);
	atomic64_inc(&sddc->load_entries);
	trace = atomic_inc_return(&dbg_cnt) <= 48;

	if (meta->kind == SDDC_KIND_REF || meta->kind == SDDC_KIND_ALIAS) {
		ref = sddc_pin_ref(sddc, (meta->kind == SDDC_KIND_REF) ?
				   meta : meta->ref);
		if (!ref) {
			atomic64_inc(&sddc->re_pin);
			atomic64_inc(&sddc->read_errors);
			goto out;
		}
		pinned = ref;

		sddc_read_stream(sddc, ref->handle, ref->wire_len,
				 sddc->dec_tmp);
		pagebuf = kmap_atomic(page);
		zstrm = zcomp_stream_get(sddc->comp);
		ret = zcomp_decompress(zstrm, sddc->dec_tmp, ref->wire_len,
				       pagebuf);
		zcomp_stream_put(sddc->comp);
		if (!ret) {
			memcpy(sddc->dbg_pg_head, pagebuf, 16);
			sddc->dbg_pg_off = ((u8 *)pagebuf)[107];
		}
		kunmap_atomic(pagebuf);
		if (ret) {
			sddc->dbg_refdec_ret = ret;
			memcpy(sddc->dbg_ref_head, sddc->dec_tmp, 16);
			sddc->dbg_stage = (meta->kind == SDDC_KIND_REF) ? 4 : 5;
			atomic64_inc(&sddc->re_plain);
			atomic64_inc(&sddc->read_errors);
		}
		goto out;
	}

	if (meta->kind != SDDC_KIND_DELTA)
		goto out;

	ref = sddc_pin_ref(sddc, meta->ref);
	if (!ref) {
		atomic64_inc(&sddc->re_pin);
		atomic64_inc(&sddc->read_errors);
		goto out;
	}
	pinned = ref;

	if (trace)
		SDDC_MARK("l-wire", meta->wire_len, 0);
	sddc_read_stream(sddc, meta->handle, meta->wire_len, sddc->dec_hdr);
	hdr = sddc->dec_hdr;
	sddc->dbg_magic = hdr->magic;
	sddc->dbg_kind = hdr->kind;
	sddc->dbg_plen = hdr->payload_len;
	sddc->dbg_wire = meta->wire_len;
	sddc->dbg_hdr_ref = hdr->ref_hash;
	sddc->dbg_ref_hash = ref->stream_hash;
	sddc->dbg_phash = hdr->payload_hash;
	sddc->dbg_calc_phash = sddc_hash_bytes(
			sddc->dec_hdr + SDDC_HDR_BYTES, hdr->payload_len);
	if (hdr->magic != SDDC_MAGIC || hdr->version != SDDC_VERSION ||
	    hdr->kind != SDDC_KIND_DELTA ||
	    hdr->payload_len + SDDC_HDR_BYTES != meta->wire_len ||
	    hdr->ref_hash != ref->stream_hash ||
	    hdr->payload_hash != sddc_hash_bytes(sddc->dec_hdr + SDDC_HDR_BYTES,
						 hdr->payload_len)) {
		sddc->dbg_stage = 1;
		atomic64_inc(&sddc->re_hdr);
		atomic64_inc(&sddc->read_errors);
		if (trace)
			SDDC_MARK("l-badhdr", hdr->payload_len, meta->wire_len);
		goto out;
	}
	if (trace)
		SDDC_MARK("l-hdrok", hdr->payload_len, 0);

	sddc_read_stream(sddc, ref->handle, ref->wire_len, sddc->dec_tmp);
	zstrm = zcomp_stream_get(sddc->comp);
	ret = zcomp_decompress(zstrm, sddc->dec_tmp, ref->wire_len,
			       sddc->dec_pair);
	sddc->dbg_refdec_ret = ret;
	if (trace)
		SDDC_MARK("l-refdec", ref->wire_len, (unsigned)-ret);
	if (!ret) {
		void *payload = sddc->dec_hdr + SDDC_HDR_BYTES;

		memcpy(sddc->dbg_ref_head, sddc->dec_pair, 16);
		if (trace)
			SDDC_MARK("l-ddec", hdr->payload_len, 0);
		ret = zcomp_decompress_delta(zstrm, payload, hdr->payload_len,
					     sddc->dec_pair,
					     sddc->dec_pair + PAGE_SIZE);
		sddc->dbg_ddec_ret = ret;
		if (trace)
			SDDC_MARK("l-dok", hdr->payload_len, (unsigned)-ret);
		if (!ret) {
			memcpy(sddc->dbg_tgt_head, sddc->dec_pair + PAGE_SIZE, 16);
			sddc->dbg_st_off = ((u8 *)sddc->dec_pair + PAGE_SIZE)[107];
			sddc->dbg_stage = 0;
			pagebuf = kmap_atomic(page);
			memcpy(pagebuf, sddc->dec_pair + PAGE_SIZE, PAGE_SIZE);
			memcpy(sddc->dbg_pg_head, pagebuf, 16);
			sddc->dbg_pg_off = ((u8 *)pagebuf)[107];
			kunmap_atomic(pagebuf);
		} else {
			sddc->dbg_stage = 3;
			atomic64_inc(&sddc->re_ddec);
			atomic64_inc(&sddc->read_errors);
		}
	} else {
		sddc->dbg_stage = 2;
		atomic64_inc(&sddc->re_refdec);
		memcpy(sddc->dbg_ref_head, sddc->dec_tmp, 16);
		atomic64_inc(&sddc->read_errors);
	}
	zcomp_stream_put(sddc->comp);

out:
	if (pinned)
		sddc_put(sddc, pinned);
	spin_unlock(&sddc->dec_lock);
	if (ret) {
		if (atomic64_read(&sddc->read_errors) > 32)
			atomic_set(&sddc->mode, 0);
		if (sddc->fail_zero) {
			void *zb = kmap_atomic(page);
			memset(zb, 0, PAGE_SIZE);
			kunmap_atomic(zb);
			ret = 0;
		}
	}
	return ret;
}

void zram_sddc_free_slot(struct zram_sddc *sddc, void *meta_ptr)
{
	struct sddc_meta *meta = meta_ptr;
	struct sddc_meta *ref = meta->ref;
	int deps;

	spin_lock(&sddc->bucket_lock);
	if (meta->kind == SDDC_KIND_REF)
		sddc_unindex(sddc, meta);
	meta->orphan = 1;
	deps = meta->dependents;
	spin_unlock(&sddc->bucket_lock);

	if (ref)
		sddc_put(sddc, ref);

	if (deps == 0)
		meta->dependents = 1;	/* route through the common release */
	if (meta->kind == SDDC_KIND_ALIAS || deps == 0)
		sddc_put(sddc, meta);
}

struct zram_sddc *zram_sddc_create(struct zs_pool *pool, struct zcomp *comp,
				   unsigned long huge_class_size)
{
	struct zram_sddc *sddc;

	sddc = kzalloc(sizeof(*sddc), GFP_KERNEL);
	if (!sddc)
		return NULL;

	sddc->pool = pool;
	sddc->comp = comp;
	sddc->huge_class_size = huge_class_size;
	spin_lock_init(&sddc->bucket_lock);
	mutex_init(&sddc->enc_lock);
	spin_lock_init(&sddc->dec_lock);
	atomic_set(&sddc->mode, 2);
	atomic_set(&sddc->next_gen, 0);
	sddc->fail_zero = false;
	sddc->free_wq = alloc_workqueue("kds-sddc-free", WQ_UNBOUND, 0);
	if (!sddc->free_wq)
		goto err;
	strscpy(sddc->comp_name, comp->name ? comp->name : "?",
		sizeof(sddc->comp_name));
	sddc->has_delta = zcomp_has_delta(comp);

	sddc->buckets = vzalloc(array_size(SDDC_BUCKETS,
					   sizeof(struct sddc_bucket)));
	sddc->enc_pair = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 1);
	sddc->enc_out = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	sddc->enc_plain = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	sddc->enc_tmp = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	sddc->enc_tmp2 = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	sddc->dec_pair = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 1);
	sddc->dec_hdr = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	sddc->dec_tmp = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	sddc->proof_pair = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 1);

	if (!sddc->buckets || !sddc->enc_pair || !sddc->enc_out ||
	    !sddc->enc_plain || !sddc->enc_tmp || !sddc->enc_tmp2 ||
	    !sddc->dec_pair || !sddc->dec_hdr ||
	    !sddc->dec_tmp || !sddc->proof_pair)
		goto err;

	return sddc;
err:
	zram_sddc_destroy(sddc);
	return NULL;
}

void zram_sddc_destroy(struct zram_sddc *sddc)
{
	if (!sddc)
		return;

	if (sddc->free_wq) {
		flush_workqueue(sddc->free_wq);
		destroy_workqueue(sddc->free_wq);
	}
	vfree(sddc->buckets);
	free_pages((unsigned long)sddc->enc_pair, 1);
	free_page((unsigned long)sddc->enc_out);
	free_page((unsigned long)sddc->enc_plain);
	free_page((unsigned long)sddc->enc_tmp);
	free_page((unsigned long)sddc->enc_tmp2);
	free_pages((unsigned long)sddc->dec_pair, 1);
	free_pages((unsigned long)sddc->proof_pair, 1);
	free_page((unsigned long)sddc->dec_hdr);
	free_page((unsigned long)sddc->dec_tmp);
	kfree(sddc);
}

int zram_sddc_mode_get(struct zram_sddc *sddc)
{
	return sddc ? atomic_read(&sddc->mode) : 0;
}

void zram_sddc_mode_set(struct zram_sddc *sddc, int mode)
{
	if (!sddc)
		return;
	if (mode < 0)
		mode = 0;
	if (mode > 4)
		mode = 4;
	atomic_set(&sddc->mode, mode);
}

ssize_t zram_sddc_stat(struct zram_sddc *sddc, char *buf, size_t len)
{
	char rh[33], th[33], ph[33], sh[33];
	int i;

	if (!sddc)
		return scnprintf(buf, len, "sddc=off\n");
	for (i = 0; i < 16; i++) {
		snprintf(rh + 2 * i, 3, "%02x", sddc->dbg_ref_head[i]);
		snprintf(th + 2 * i, 3, "%02x", sddc->dbg_tgt_head[i]);
		snprintf(ph + 2 * i, 3, "%02x", sddc->dbg_pg_head[i]);
		snprintf(sh + 2 * i, 3, "%02x", sddc->dbg_store_head[i]);
	}
	rh[32] = 0;
	th[32] = 0;
	ph[32] = 0;
	sh[32] = 0;
	return scnprintf(buf, len,
		"alias=%llu delta=%llu fallback=%llu read_err=%llu proof_fail=%llu load_entries=%llu\n"
		"re_pin=%llu re_plain=%llu re_hdr=%llu re_refdec=%llu re_ddec=%llu refverify=%llu\n"
		"store=%llu cand=%llu miss_small=%llu miss_refdec=%llu miss_enc=%llu miss_size=%llu miss_proof=%llu miss_wire=%llu miss_class=%llu meta_alive=%lld mode=%d comp=%s has_delta=%d tried=%llu nodelta=%llu\n"
		"alive_ref=%lld alive_alias=%lld alive_delta=%lld wq_pending=%lld zs_fail=%llu ref_cap=%llu delta_cap=%llu\n"
		"stage=%u magic=%08x kind=%u plen=%u wire=%u\n"
		"ref_hash=%08x hdr_ref=%08x phash=%08x calc_phash=%08x\n"
		"refdec_ret=%d ddec_ret=%d last_slen=%u last_dlen=%u last_wire=%u\n"
		"dry_tried=%llu dry_ok=%llu dry_refdec=%llu dry_ddec=%llu dry_rdiff=%llu\n"
		"store_refdec_ret=%d store_wire=%u store_head=%s\n"
		"ref_head=%s tgt_head=%s pg_head=%s st_off=%u pg_off=%u\n",
		(u64)atomic64_read(&sddc->alias_hits),
		(u64)atomic64_read(&sddc->delta_hits),
		(u64)atomic64_read(&sddc->fallbacks),
		(u64)atomic64_read(&sddc->read_errors),
		(u64)atomic64_read(&sddc->proof_failures),
		(u64)atomic64_read(&sddc->load_entries),
		(u64)atomic64_read(&sddc->re_pin),
		(u64)atomic64_read(&sddc->re_plain),
		(u64)atomic64_read(&sddc->re_hdr),
		(u64)atomic64_read(&sddc->re_refdec),
		(u64)atomic64_read(&sddc->re_ddec),
		(u64)atomic64_read(&sddc->ref_verify_fail),
		(u64)atomic64_read(&sddc->store_total),
		(u64)atomic64_read(&sddc->cand_total),
		(u64)atomic64_read(&sddc->miss_small),
		(u64)atomic64_read(&sddc->miss_refdec),
		(u64)atomic64_read(&sddc->miss_delta_enc),
		(u64)atomic64_read(&sddc->miss_delta_size),
		(u64)atomic64_read(&sddc->miss_proof),
		(u64)atomic64_read(&sddc->miss_wire),
		(u64)atomic64_read(&sddc->miss_class),
		(long long)atomic64_read(&sddc->meta_alive),
		atomic_read(&sddc->mode),
		sddc->comp_name, (int)sddc->has_delta,
		(u64)atomic64_read(&sddc->delta_tried),
		(u64)atomic64_read(&sddc->miss_nodelta),
		(long long)atomic64_read(&sddc->alive_ref),
		(long long)atomic64_read(&sddc->alive_alias),
		(long long)atomic64_read(&sddc->alive_delta),
		(long long)atomic64_read(&sddc->wq_pending),
		(u64)atomic64_read(&sddc->zs_fail),
		(u64)atomic64_read(&sddc->ref_cap_skip),
		(u64)atomic64_read(&sddc->delta_cap_skip),
		sddc->dbg_stage, sddc->dbg_magic, sddc->dbg_kind,
		sddc->dbg_plen, sddc->dbg_wire,
		sddc->dbg_ref_hash, sddc->dbg_hdr_ref,
		sddc->dbg_phash, sddc->dbg_calc_phash,
		sddc->dbg_refdec_ret, sddc->dbg_ddec_ret,
		sddc->last_slen, sddc->last_dlen, sddc->last_wire,
		(u64)atomic64_read(&sddc->dry_tried),
		(u64)atomic64_read(&sddc->dry_ok),
		(u64)atomic64_read(&sddc->dry_refdec),
		(u64)atomic64_read(&sddc->dry_ddec),
			(u64)atomic64_read(&sddc->dry_rdiff),
		sddc->dbg_store_refdec_ret, sddc->dbg_store_wire, sh,
		rh, th, ph, sddc->dbg_st_off, sddc->dbg_pg_off);
}
