/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2014 Sergey Senozhatsky.
 */

#ifndef _ZCOMP_H_
#define _ZCOMP_H_
#include <linux/local_lock.h>

/*
 * Direct (non crypto API) compression backend.  Needed by algorithms that
 * carry more per-cpu state than the crypto compress API can hold, or that
 * implement delta (dictionary) compression, which the crypto API has no
 * interface for.
 *
 * ->decompress may run from the zram read path while the slot bit spinlock
 * is held, so it must never sleep.
 */
struct zcomp_backend_ops {
	int (*compress)(void *ctx, const void *src, size_t src_len,
			void *dst, size_t *dst_len);
	int (*decompress)(void *ctx, const void *src, size_t src_len,
			void *dst, size_t *dst_len);
	/* Optional dictionary compression against a reference page. */
	int (*compress_delta)(void *ctx, const void *ref, const void *src,
			      size_t src_len, void *dst, size_t *dst_len,
			      size_t out_max);
	int (*decompress_delta)(void *ctx, const void *src, size_t src_len,
				const void *ref, void *dst, size_t *dst_len);
	/* Size of the per-cpu context, 0 when the backend needs none. */
	size_t ctx_size;
};

struct zcomp_strm {
	/* The members ->buffer and ->tfm are protected by ->lock. */
	local_lock_t lock;
	/* compression/decompression buffer */
	void *buffer;
	struct crypto_comp *tfm;
	/* direct backend context (per-cpu) */
	void *ctx;
	/* non-NULL when this stream uses a direct backend */
	const struct zcomp_backend_ops *ops;
};

/* dynamic per-device compression frontend */
struct zcomp {
	struct zcomp_strm __percpu *stream;
	const char *name;
	const struct zcomp_backend_ops *ops;
	struct hlist_node node;
};

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node);
int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node);
ssize_t zcomp_available_show(const char *comp, char *buf);
bool zcomp_available_algorithm(const char *comp);

struct zcomp *zcomp_create(const char *comp);
void zcomp_destroy(struct zcomp *comp);

struct zcomp_strm *zcomp_stream_get(struct zcomp *comp);
void zcomp_stream_put(struct zcomp *comp);

int zcomp_compress(struct zcomp_strm *zstrm,
		const void *src, unsigned int *dst_len);

int zcomp_decompress(struct zcomp_strm *zstrm,
		const void *src, unsigned int src_len, void *dst);

bool zcomp_set_max_streams(struct zcomp *comp, int num_strm);

/* Delta (dictionary) compression; available only on kds style backends. */
bool zcomp_has_delta(struct zcomp *comp);
int zcomp_compress_delta(struct zcomp_strm *zstrm, const void *ref,
		const void *src, unsigned int *dst_len, unsigned int out_max);
int zcomp_decompress_delta(struct zcomp_strm *zstrm, const void *src,
		unsigned int src_len, const void *ref, void *dst);

#endif /* _ZCOMP_H_ */
