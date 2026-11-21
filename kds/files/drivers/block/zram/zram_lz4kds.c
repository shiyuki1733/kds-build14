// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * LZ4KDS ("kds") direct zcomp backend.
 *
 * lz4kd is a plain 4 KiB block codec which also implements dictionary (delta)
 * compression: lz4kd_encode_delta() compresses a page using a reference page
 * as dictionary, which is what SDDC delta slots need.  The crypto compress
 * API has no delta interface and cannot carry the codec's per-cpu state, so
 * the codec is driven directly.
 *
 * The reference and target pages must be physically contiguous for the delta
 * calls (see lz4kd_encode_delta() documentation); the SDDC layer hands us a
 * paired 8 KiB buffer and copies the reference page in front of the target.
 */

#include <linux/kernel.h>
#include <linux/lz4kd.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>

#include "zcomp.h"

/* Matches lz4kd_encode_state_bytes_min(): 13-bit hash + offset table. */
#define LZ4KDS_ENC_STATE_BYTES	(1U << 15)

static int lz4kds_compress(void *ctx, const void *src, size_t src_len,
			   void *dst, size_t *dst_len)
{
	int ret;

	if (src_len != PAGE_SIZE)
		return -EINVAL;

	ret = lz4kd_encode(ctx, src, dst, src_len, *dst_len, 0);
	if (ret <= 0)
		return -EINVAL;
	*dst_len = ret;
	return 0;
}

static int lz4kds_decompress(void *ctx, const void *src, size_t src_len,
			     void *dst, size_t *dst_len)
{
	int ret;

	ret = lz4kd_decode(src, dst, src_len, *dst_len);
	if (ret <= 0)
		return -EINVAL;
	*dst_len = ret;
	return 0;
}

static int lz4kds_compress_delta(void *ctx, const void *ref, const void *src,
				 size_t src_len, void *dst, size_t *dst_len,
				 size_t out_max)
{
	int ret;

	if (src_len != PAGE_SIZE)
		return -EINVAL;

	ret = lz4kd_encode_delta(ctx, ref, src, dst, src_len, *dst_len,
				 out_max);
	if (ret <= 0)
		return -EINVAL;
	*dst_len = ret;
	return 0;
}

static int lz4kds_decompress_delta(void *ctx, const void *src, size_t src_len,
				   const void *ref, void *dst, size_t *dst_len)
{
	int ret;

	ret = lz4kd_decode_delta(src, ref, dst, src_len, *dst_len);
	if (ret <= 0)
		return -EINVAL;
	*dst_len = ret;
	return 0;
}

const struct zcomp_backend_ops lz4kds_zcomp_ops = {
	.compress = lz4kds_compress,
	.decompress = lz4kds_decompress,
	.compress_delta = lz4kds_compress_delta,
	.decompress_delta = lz4kds_decompress_delta,
	.ctx_size = LZ4KDS_ENC_STATE_BYTES,
};

/* Same codec, SDDC alias dedup only (no delta), for A/B on-device testing. */
const struct zcomp_backend_ops lz4kdsn_zcomp_ops = {
	.compress = lz4kds_compress,
	.decompress = lz4kds_decompress,
	.ctx_size = LZ4KDS_ENC_STATE_BYTES,
};
