/*
 * BSD 3-Clause New License (https://spdx.org/licenses/BSD-3-Clause.html)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 2016-2018, Klara Inc.
 * Copyright (c) 2016-2018, Allan Jude
 * Copyright (c) 2018-2020, Sebastian Gottschall
 * Copyright (c) 2019-2020, Michael Niewöhner
 * Copyright (c) 2020, The FreeBSD Foundation [1]
 *
 * [1] Portions of this software were developed by Allan Jude
 *     under sponsorship from the FreeBSD Foundation.
 */

#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/zfs_context.h>
#include <sys/zio_compress.h>
#include <sys/spa.h>
#include <sys/zstd/zstd.h>

#define	ZSTD_STATIC_LINKING_ONLY
#include "lib/zstd.h"
#include "lib/common/zstd_errors.h"

static int zstd_lz4_pass = 1;
//static int zstd_firstpass_level = ZIO_ZSTD_LEVEL_1;
static int zstd_firstpass_mode = 1;
static int zstd_cutoff_level = ZIO_ZSTD_LEVEL_3;
static int zstd_zstd_pass = 1;
static int zstd_hard_moed = 0;
static int zstd_lz4_shift_size = 3;
//static int zstd_zstd_shift_size = 3;
static int zstd_ea_division_mode = 1;
static int zstd_ea_divisor = 2;
static int zstd_ea_level_factor = 2;
//static unsigned int zstd_abort_size = 131072;
// scaled by 2 because we're starting at zstd-3 now
static unsigned int zstd_abort_size = (131072);

kstat_t *zstd_ksp = NULL;


typedef struct zstd_stats {
	kstat_named_t	zstd_stat_alloc_fail;
	kstat_named_t	zstd_stat_com_alloc_fail;
	kstat_named_t	zstd_stat_dec_alloc_fail;
	kstat_named_t	zstd_stat_com_inval;
	kstat_named_t	zstd_stat_dec_inval;
	kstat_named_t	zstd_stat_dec_header_inval;
	kstat_named_t	zstd_stat_com_fail;
	kstat_named_t	zstd_stat_dec_fail;
/*
 * LZ4 first-pass early abort verdict
 */
	kstat_named_t	zstd_stat_lz4pass_allowed;
	kstat_named_t	zstd_stat_lz4pass_rejected;
/*
 * zstd-1 second-pass early abort verdict
 */
	kstat_named_t	zstd_stat_zstdpass_allowed;
	kstat_named_t	zstd_stat_zstdpass_rejected;
/*
 * We excluded this from early abort for some reason
 */
	kstat_named_t	zstd_stat_passignored;
	kstat_named_t	zstd_stat_passignored_size;
	kstat_named_t	zstd_stat_buffers;
	kstat_named_t	zstd_stat_size;
} zstd_stats_t;

static zstd_stats_t zstd_stats = {
	{ "alloc_fail",			KSTAT_DATA_UINT64 },
	{ "compress_alloc_fail",	KSTAT_DATA_UINT64 },
	{ "decompress_alloc_fail",	KSTAT_DATA_UINT64 },
	{ "compress_level_invalid",	KSTAT_DATA_UINT64 },
	{ "decompress_level_invalid",	KSTAT_DATA_UINT64 },
	{ "decompress_header_invalid",	KSTAT_DATA_UINT64 },
	{ "compress_failed",		KSTAT_DATA_UINT64 },
	{ "decompress_failed",		KSTAT_DATA_UINT64 },
	{ "lz4pass_allowed",		KSTAT_DATA_UINT64 },
	{ "lz4pass_rejected",		KSTAT_DATA_UINT64 },
	{ "zstdpass_allowed",		KSTAT_DATA_UINT64 },
	{ "zstdpass_rejected",		KSTAT_DATA_UINT64 },
	{ "passignored",		KSTAT_DATA_UINT64 },
	{ "passignored_size",		KSTAT_DATA_UINT64 },
	{ "buffers",			KSTAT_DATA_UINT64 },
	{ "size",			KSTAT_DATA_UINT64 },
};

#ifdef _KERNEL
static int
kstat_zstd_update(kstat_t *ksp, int rw) {

        ASSERT(ksp != NULL);

        if (rw == KSTAT_WRITE && ksp == zstd_ksp) {
        	ZSTDSTAT_ZERO(zstd_stat_alloc_fail);
        	ZSTDSTAT_ZERO(zstd_stat_com_alloc_fail);
        	ZSTDSTAT_ZERO(zstd_stat_dec_alloc_fail);
        	ZSTDSTAT_ZERO(zstd_stat_com_inval);
        	ZSTDSTAT_ZERO(zstd_stat_dec_inval);
        	ZSTDSTAT_ZERO(zstd_stat_dec_header_inval);
        	ZSTDSTAT_ZERO(zstd_stat_com_fail);
        	ZSTDSTAT_ZERO(zstd_stat_dec_fail);
        	ZSTDSTAT_ZERO(zstd_stat_lz4pass_allowed);
        	ZSTDSTAT_ZERO(zstd_stat_lz4pass_rejected);
        	ZSTDSTAT_ZERO(zstd_stat_zstdpass_allowed);
        	ZSTDSTAT_ZERO(zstd_stat_zstdpass_rejected);
        	ZSTDSTAT_ZERO(zstd_stat_passignored);
        	ZSTDSTAT_ZERO(zstd_stat_passignored_size);
        }

        return (0);

}
#endif

/*
 * structure for allocation metadata, since zstd memory interface
 * expects malloc/free-like semantics instead of vmem-like
 */
struct zstd_kmem_hdr {
	size_t kmem_size;
	char data[];
};

struct zstd_levelmap {
	int16_t zstd_level;
	enum zio_zstd_levels level;
};

/*
 * ZSTD memory handlers
 */
static void *zstd_alloc_cb(void *opaque, size_t size);
static void zstd_free_cb(void *opaque, void *ptr);

/* Compression memory handler */
static const ZSTD_customMem zstd_cctx_custommem = {
	.customAlloc = zstd_alloc_cb,
	.customFree = zstd_free_cb,
	.opaque = (void*)0,
};

/* Decompression memory handler */
static const ZSTD_customMem zstd_dctx_custommem = {
	.customAlloc = zstd_alloc_cb,
	.customFree = zstd_free_cb,
	/* "try hard" since a failure on decompression path cascades to user: */
	.opaque = (void*)1,
};

/* Level map for converting ZFS internal levels to ZSTD levels and vice versa */
static struct zstd_levelmap zstd_levels[] = {
	{ZIO_ZSTD_LEVEL_1, ZIO_ZSTD_LEVEL_1},
	{ZIO_ZSTD_LEVEL_2, ZIO_ZSTD_LEVEL_2},
	{ZIO_ZSTD_LEVEL_3, ZIO_ZSTD_LEVEL_3},
	{ZIO_ZSTD_LEVEL_4, ZIO_ZSTD_LEVEL_4},
	{ZIO_ZSTD_LEVEL_5, ZIO_ZSTD_LEVEL_5},
	{ZIO_ZSTD_LEVEL_6, ZIO_ZSTD_LEVEL_6},
	{ZIO_ZSTD_LEVEL_7, ZIO_ZSTD_LEVEL_7},
	{ZIO_ZSTD_LEVEL_8, ZIO_ZSTD_LEVEL_8},
	{ZIO_ZSTD_LEVEL_9, ZIO_ZSTD_LEVEL_9},
	{ZIO_ZSTD_LEVEL_10, ZIO_ZSTD_LEVEL_10},
	{ZIO_ZSTD_LEVEL_11, ZIO_ZSTD_LEVEL_11},
	{ZIO_ZSTD_LEVEL_12, ZIO_ZSTD_LEVEL_12},
	{ZIO_ZSTD_LEVEL_13, ZIO_ZSTD_LEVEL_13},
	{ZIO_ZSTD_LEVEL_14, ZIO_ZSTD_LEVEL_14},
	{ZIO_ZSTD_LEVEL_15, ZIO_ZSTD_LEVEL_15},
	{ZIO_ZSTD_LEVEL_16, ZIO_ZSTD_LEVEL_16},
	{ZIO_ZSTD_LEVEL_17, ZIO_ZSTD_LEVEL_17},
	{ZIO_ZSTD_LEVEL_18, ZIO_ZSTD_LEVEL_18},
	{ZIO_ZSTD_LEVEL_19, ZIO_ZSTD_LEVEL_19},
	{-1, ZIO_ZSTD_LEVEL_FAST_1},
	{-2, ZIO_ZSTD_LEVEL_FAST_2},
	{-3, ZIO_ZSTD_LEVEL_FAST_3},
	{-4, ZIO_ZSTD_LEVEL_FAST_4},
	{-5, ZIO_ZSTD_LEVEL_FAST_5},
	{-6, ZIO_ZSTD_LEVEL_FAST_6},
	{-7, ZIO_ZSTD_LEVEL_FAST_7},
	{-8, ZIO_ZSTD_LEVEL_FAST_8},
	{-9, ZIO_ZSTD_LEVEL_FAST_9},
	{-10, ZIO_ZSTD_LEVEL_FAST_10},
	{-20, ZIO_ZSTD_LEVEL_FAST_20},
	{-30, ZIO_ZSTD_LEVEL_FAST_30},
	{-40, ZIO_ZSTD_LEVEL_FAST_40},
	{-50, ZIO_ZSTD_LEVEL_FAST_50},
	{-60, ZIO_ZSTD_LEVEL_FAST_60},
	{-70, ZIO_ZSTD_LEVEL_FAST_70},
	{-80, ZIO_ZSTD_LEVEL_FAST_80},
	{-90, ZIO_ZSTD_LEVEL_FAST_90},
	{-100, ZIO_ZSTD_LEVEL_FAST_100},
	{-500, ZIO_ZSTD_LEVEL_FAST_500},
	{-1000, ZIO_ZSTD_LEVEL_FAST_1000},
};

/* Object-pool implementation */

#define	OBJPOOL_TIMEOUT_SEC 15

typedef struct {
	/* Access to 'count' and 'list' must hold 'listlock' */
	kmutex_t listlock;
	int count;
	void **list;

	int64_t last_accessed_jiffy; /* for idle-reap */

	/* Must be set by user */
	void*(*obj_alloc)(void);
	void(*obj_free)(void*);
	void(*obj_reset)(void*);
	const char * const pool_name;
} objpool_t;

static void objpool_init(objpool_t *const objpool);
static void objpool_reap(objpool_t *const objpool);
static void objpool_destroy(objpool_t *const objpool);

static void* obj_grab(objpool_t *const objpool);
static void obj_ungrab(objpool_t *const objpool, void* const obj);
/*
 * The library zstd code expects these if ADDRESS_SANITIZER gets defined,
 * and while ASAN does this, KASAN defines that and does not. So to avoid
 * changing the external code, we do this.
 */
#if defined(ZFS_ASAN_ENABLED)
#define	ADDRESS_SANITIZER 1
#endif
#if defined(_KERNEL) && defined(ADDRESS_SANITIZER)
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size) {};
void __asan_poison_memory_region(void const volatile *addr, size_t size) {};
#endif


static void
objpool_reset_idle_timer(objpool_t *const objpool)
{
	int64_t const now_jiffy = ddi_get_lbolt64();
	objpool->last_accessed_jiffy = now_jiffy;
}

static void
objpool_init(objpool_t *const objpool)
{
	mutex_init(&objpool->listlock, NULL, MUTEX_DEFAULT, NULL);

	objpool->count = 0;
	objpool->list = NULL;
	objpool_reset_idle_timer(objpool);
}

static void
objpool_clearunused(objpool_t *const objpool)
{
	mutex_enter(&objpool->listlock);
	for (int i = 0; i < objpool->count; ++i) {
		/* if ANY object is still in use then don't do anything */
		if (unlikely(NULL == objpool->list[i])) {
			mutex_exit(&objpool->listlock);
			return;
		}
	}
	for (int i = 0; i < objpool->count; ++i) {
		objpool->obj_free(objpool->list[i]);
	}
	if (objpool->count > 0) {
		ASSERT3P(objpool->list, !=, NULL);
		kmem_free(objpool->list, sizeof (void *) * objpool->count);
		objpool->list = NULL;
	} else {
		ASSERT3P(objpool->list, ==, NULL);
	}
	objpool->count = 0;
	mutex_exit(&objpool->listlock);
}

static void
objpool_reap(objpool_t *const objpool)
{
	int64_t now_jiffy = ddi_get_lbolt64();
	if (objpool->last_accessed_jiffy > now_jiffy ||
	    now_jiffy - objpool->last_accessed_jiffy
	    > SEC_TO_TICK(OBJPOOL_TIMEOUT_SEC)) {
		objpool_clearunused(objpool);
		objpool_reset_idle_timer(objpool);
	}
}

static void __exit
objpool_destroy(objpool_t *const objpool)
{
	objpool_clearunused(objpool);
	VERIFY3U(objpool->count, ==, 0);

	if (objpool->count == 0) {
		mutex_destroy(&objpool->listlock);
	}
}

static void*
obj_grab(objpool_t *const objpool)
{
	void* grabbed_obj = NULL;
	mutex_enter(&objpool->listlock);
	const int objcount = objpool->count;
	for (int i = 0; i < objcount; ++i) {
		if ((grabbed_obj = objpool->list[i]) != NULL) {
			objpool->list[i] = NULL;
			break;
		}
	}
	if (likely(grabbed_obj != NULL)) {
		/* grabbed pooled object; reset it to a reusable state */
		objpool->obj_reset(grabbed_obj);
	} else {
		grabbed_obj = objpool->obj_alloc();
		if (grabbed_obj != NULL) {
			int newlistbytes = sizeof (void *)
			    * (objpool->count + 1);
			void **newlist = kmem_alloc(newlistbytes, KM_NOSLEEP);
			if (newlist != NULL) {
				newlist[0] = NULL;
				if (objpool->count > 0) {
					ASSERT3P(objpool->list, !=, NULL);
					memcpy(&newlist[1], &objpool->list[0],
					    sizeof (void *) * (objpool->count));
					kmem_free(objpool->list, sizeof (void *)
					    * (objpool->count));
				} else {
					ASSERT3P(objpool->list, ==, NULL);
				}
				objpool->list = newlist;
				++objpool->count;
			} else {
				/*
				 * Failed to grow the pool.
				 * This is okay; we can still return the new
				 * object, but the next ungrab()'d object might
				 * not find a spare pool slot (in which case the
				 * object will just be destroyed cleanly when
				 * ungrab()'d).
				 */
			}
		} else {
			/* failed to alloc new object, will return NULL */
		}
	}
	mutex_exit(&objpool->listlock);
	return (grabbed_obj);
}

static void
obj_ungrab(objpool_t *const objpool, void* const obj)
{
	ASSERT3P(obj, !=, NULL);
	mutex_enter(&objpool->listlock);
	int const objcount = objpool->count;
#ifdef DEBUG
	for (int i = 0; i < objcount; ++i) {
		/*
		 * if the ungrab'd object is already in the pool
		 * then something's gone very wrong.
		 */
		ASSERT3P(objpool->list[i], !=, obj);
	}
#endif /* DEBUG */
	boolean_t got_slot = B_FALSE;
	for (int i = 0; i < objcount; ++i) {
		if (objpool->list[i] == NULL) {
			objpool->list[i] = obj;
			got_slot = B_TRUE;
			break;
		}
	}
	mutex_exit(&objpool->listlock);
	objpool_reset_idle_timer(objpool);
	if (unlikely(!got_slot)) {
		/*
		 * If there's no space in the pool to keep it,
		 * just destroy the object now.
		 */
		objpool->obj_free(obj);
	}
}

/* Callbacks for our compression/decompression object pools */

static void *
cctx_alloc(void)
{
	return (ZSTD_createCCtx_advanced(zstd_cctx_custommem));
}
static void
cctx_free(void *ptr)
{
	(void) ZSTD_freeCCtx(ptr);
}
static void
cctx_reset(void *ptr)
{
	/*
	 * note: compressor needs to reset *stream* only on error,
	 * we reset *parameters* always
	 */
	(void) ZSTD_CCtx_reset(ptr, ZSTD_reset_parameters);
}

static void *
dctx_alloc(void)
{
	return (ZSTD_createDCtx_advanced(zstd_dctx_custommem));
}
static void
dctx_free(void *ptr)
{
	(void) ZSTD_freeDCtx(ptr);
}
static void
dctx_reset(void *ptr)
{
	/*
	 * note: decompressor needs to reset *stream* only on error,
	 * we reset *parameters* always
	 */
	(void) ZSTD_DCtx_reset(ptr, ZSTD_reset_parameters);
}

static objpool_t cctx_pool = {
	.obj_alloc = cctx_alloc,
	.obj_free  = cctx_free,
	.obj_reset = cctx_reset,
	.pool_name = "zstdCctx"
	};

static objpool_t dctx_pool = {
	.obj_alloc = dctx_alloc,
	.obj_free  = dctx_free,
	.obj_reset = dctx_reset,
	.pool_name = "zstdDctx"
	};


/* Convert ZFS internal enum to ZSTD level */
static int
zstd_enum_to_level(enum zio_zstd_levels level, int16_t *zstd_level)
{
	if (level > 0 && level <= ZIO_ZSTD_LEVEL_19) {
		*zstd_level = zstd_levels[level - 1].zstd_level;
		return (0);
	}
	if (level >= ZIO_ZSTD_LEVEL_FAST_1 &&
	    level <= ZIO_ZSTD_LEVEL_FAST_1000) {
		*zstd_level = zstd_levels[level - ZIO_ZSTD_LEVEL_FAST_1
		    + ZIO_ZSTD_LEVEL_19].zstd_level;
		return (0);
	}

	/* Invalid/unknown zfs compression enum - this should never happen. */
	return (1);
}


size_t
zfs_zstd_compress_wrap(void *s_start, void *d_start, size_t s_len, size_t d_len,
    int level)
{
	int16_t zstd_level;
	if (zstd_enum_to_level(level, &zstd_level)) {
		ZSTDSTAT_BUMP(zstd_stat_com_inval);
		return (s_len);
	}
	/*
	 * A zstd early abort heuristic.
	 *
	 * - Zeroth, if this is <= zstd-3, or < zstd_abort_size (currently
	 *   128k), don't try any of this, just go.
	 *   (because experimentally that was a reasonable cutoff for a perf win
	 *   with tiny ratio change)
	 * - First, we try LZ4 compression, and if it doesn't early abort, we
	 *   jump directly to whatever compression level we intended to try.
	 * - Second, we try zstd-1 - if that errors out (usually, but not
	 *   exclusively, if it would overflow), we give up early.
	 *
	 *   If it works, instead we go on and compress anyway.
	 *
	 * Why two passes? LZ4 alone gets you a lot of the way, but on highly
	 * compressible data, it was losing up to 8.5% of the compressed
	 * savings versus no early abort, and all the zstd-fast levels are
	 * worse indications on their own than LZ4, and don't improve the LZ4
	 * pass noticably if stacked like this.
	 */
	/*
	 * A new experiment - a scaling factor.
	 * Obviously zstd scales worse than linearly as level increases. So
	 * presumably the point where it's cheaper to just do the compression
	 * is going to change too.
	 *
	 * The thinking goes:
	 * - zstd-2 is too close to zstd-1 for this to ever make sense
	 * - starting with zstd-3, let's try a cutoff of
	 *  - (zstd_abort_size) / (zstd_scaling_factor)
	 *  where zstd_scaling_factor = 
	 *    (((zstd_level - zstd_cutoff_level) / zstd_ea_level_factor) + 1)
	 *  So, for concrete example, with zstd_ea_level_factor = 2, we'd expect
	 *  a cutoff of zstd_abort_size at zstd-3, zstd_abort_size / 2 at 4-5,
	 *  zstd_abort_size / 4 at 6-7, and so on. 
	 *  Naturally, this is going to need to have a hard cutoff somewhere,
	 *  since this pass probably ~never makes sense below, say, 4k or 8k.
	 */
	size_t actual_abort_size = zstd_abort_size;
	// want it to only compress at 256k+ for zstd-3, but a default of 128k
	// below that even with the pass off.
#if 0
	if (zstd_level == zstd_cutoff_level)
		actual_abort_size *= 2;
	if (zstd_ea_division_mode && zstd_level >= zstd_cutoff_level) {
		int wip = ((zstd_level+1 - zstd_cutoff_level) / (zstd_ea_level_factor));
		while (wip > 0) {
			actual_abort_size /= zstd_ea_divisor;
			wip--;
		}
//		zfs_dbgmsg("Abort debug: zstd_level %d zstd_cutoff_level %d actual_abort_size %lu wip %d", zstd_level, zstd_cutoff_level, actual_abort_size, ((zstd_level+1 - zstd_cutoff_level) / (zstd_ea_level_factor)));
		if (actual_abort_size < 8193)
			actual_abort_size = 8193;
//		zfs_dbgmsg("Abort debug: zstd_level %d zstd_cutoff_level %d actual_abort_size %lu wip %d", zstd_level, zstd_cutoff_level, actual_abort_size, ((zstd_level+1 - zstd_cutoff_level) / (zstd_ea_level_factor)));
	}
#endif
	if ((zstd_lz4_pass > 0 && zstd_level >= zstd_cutoff_level && s_len >= actual_abort_size) ||( zstd_hard_moed > 0)) {
		int pass_len = 1;
		int lz4_len = (s_len) - (s_len >> zstd_lz4_shift_size);
		pass_len = lz4_compress_zfs(s_start, d_start, s_len, lz4_len, 0);
		if (pass_len < lz4_len && !zstd_hard_moed) {
			ZSTDSTAT_BUMP(zstd_stat_lz4pass_allowed);
			goto keep_trying;
		}
		ZSTDSTAT_BUMP(zstd_stat_lz4pass_rejected);
		if (!zstd_zstd_pass) {
			// if zstd_hard_moed is set, we don't want to trigger abort,
			// just compress the shit out of things.
			if (!zstd_hard_moed)
				return (s_len);
			else
				goto keep_trying;
		}
//		int zstd_len = (s_len) - (s_len >> zstd_zstd_shift_size);
		int zstd_len = d_len;
		int firstpass_level = 1;
		switch (zstd_firstpass_mode) {
			case 3:
				firstpass_level=-420;
				break;
			case 2:
				firstpass_level=2;
				zfs_fallthrough;
			case 1:
				zfs_fallthrough;
			default:
				break;
		}
		pass_len = zfs_zstd_compress(s_start, d_start, s_len, zstd_len,
		    firstpass_level);
		if ((pass_len == s_len || pass_len <= 0 || pass_len > zstd_len) && !zstd_hard_moed) {
			ZSTDSTAT_BUMP(zstd_stat_zstdpass_rejected);
			return (s_len);
		}
		ZSTDSTAT_BUMP(zstd_stat_zstdpass_allowed);
	} else {
		ZSTDSTAT_BUMP(zstd_stat_passignored);
		if (s_len < actual_abort_size) {
			ZSTDSTAT_BUMP(zstd_stat_passignored_size);
		}
	}
keep_trying:
	return (zfs_zstd_compress(s_start, d_start, s_len, d_len, level));

}

/* Compress block using zstd */
size_t
zfs_zstd_compress(void *s_start, void *d_start, size_t s_len, size_t d_len,
    int level)
{
	size_t c_len;
	int16_t zstd_level;
	zfs_zstdhdr_t *hdr;
	ZSTD_CCtx *cctx;

	hdr = (zfs_zstdhdr_t *)d_start;

	/* Skip compression if the specified level is invalid */
	if (zstd_enum_to_level(level, &zstd_level) && (level != -420)) {
		ZSTDSTAT_BUMP(zstd_stat_com_inval);
		return (s_len);
	}
	if (level == -420) {
		zstd_enum_to_level(2, &zstd_level);
	}

	ASSERT3U(d_len, >=, sizeof (*hdr));
	ASSERT3U(d_len, <=, s_len);
	ASSERT3U(zstd_level, !=, 0);

	cctx = obj_grab(&cctx_pool);

	/*
	 * Out of kernel memory, gently fall through - this will disable
	 * compression in zio_compress_data
	 */
	if (unlikely(!cctx)) {
		ZSTDSTAT_BUMP(zstd_stat_com_alloc_fail);
		return (s_len);
	}
	/* Set the compression level */
	ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, zstd_level);

	if (level == -420) {
		// tweak it to hell
		ZSTD_compressionParameters newevil = 
		    { 21, 15, 16,  1,  6,  0, ZSTD_fast    };  /* level  2 */
//		    { 21, 13, 14,  1,  7,  0, ZSTD_fast   };  /* level  3, >256k, diff compressor */
		//  { 19, 13, 14,  1,  7,  0, ZSTD_fast    },  /* level  1 */

		ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, newevil.windowLog);
		ZSTD_CCtx_setParameter(cctx, ZSTD_c_hashLog, newevil.hashLog);
		ZSTD_CCtx_setParameter(cctx, ZSTD_c_chainLog, newevil.chainLog);
		ZSTD_CCtx_setParameter(cctx, ZSTD_c_searchLog, newevil.searchLog);
		ZSTD_CCtx_setParameter(cctx, ZSTD_c_minMatch, newevil.minMatch);
		ZSTD_CCtx_setParameter(cctx, ZSTD_c_targetLength, newevil.targetLength);
		ZSTD_CCtx_setParameter(cctx, ZSTD_c_strategy, newevil.strategy);
		ZSTD_CCtx_setParameter(cctx, ZSTD_c_experimentalParam7, s_len);
	}
	/* Use the "magicless" zstd header which saves us 4 header bytes */
	ZSTD_CCtx_setParameter(cctx, ZSTD_c_format, ZSTD_f_zstd1_magicless);

	/*
	 * Disable redundant checksum calculation and content size storage since
	 * this is already done by ZFS itself.
	 */
	ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 0);
	ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0);

	c_len = ZSTD_compress2(cctx,
	    hdr->data,
	    d_len - sizeof (*hdr),
	    s_start, s_len);

	if (ZSTD_isError(c_len)) {
		(void) ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
	}

	obj_ungrab(&cctx_pool, cctx);

	/* Error in the compression routine, disable compression. */
	if (ZSTD_isError(c_len)) {
		/*
		 * If we are aborting the compression because the saves are
		 * too small, that is not a failure. Everything else is a
		 * failure, so increment the compression failure counter.
		 */
		int err = ZSTD_getErrorCode(c_len);
		if (err != ZSTD_error_dstSize_tooSmall) {
			ZSTDSTAT_BUMP(zstd_stat_com_fail);
			zfs_dbgmsg("Error: %s", ZSTD_getErrorString(err));
		}
		return (s_len);
	}

	/*
	 * Encode the compressed buffer size at the start. We'll need this in
	 * decompression to counter the effects of padding which might be added
	 * to the compressed buffer and which, if unhandled, would confuse the
	 * hell out of our decompression function.
	 */
	hdr->c_len = BE_32(c_len);

	/*
	 * Check version for overflow.
	 * The limit of 24 bits must not be exceeded. This allows a maximum
	 * version 1677.72.15 which we don't expect to be ever reached.
	 */
	ASSERT3U(ZSTD_VERSION_NUMBER, <=, 0xFFFFFF);

	/*
	 * Encode the compression level as well. We may need to know the
	 * original compression level if compressed_arc is disabled, to match
	 * the compression settings to write this block to the L2ARC.
	 *
	 * Encode the actual level, so if the enum changes in the future, we
	 * will be compatible.
	 *
	 * The upper 24 bits store the ZSTD version to be able to provide
	 * future compatibility, since new versions might enhance the
	 * compression algorithm in a way, where the compressed data will
	 * change.
	 *
	 * As soon as such incompatibility occurs, handling code needs to be
	 * added, differentiating between the versions.
	 */
	zfs_set_hdrversion(hdr, ZSTD_VERSION_NUMBER);
	zfs_set_hdrlevel(hdr, level);
	hdr->raw_version_level = BE_32(hdr->raw_version_level);

	return (c_len + sizeof (*hdr));
}

/* Decompress block using zstd and return its stored level */
int
zfs_zstd_decompress_level(void *s_start, void *d_start, size_t s_len,
    size_t d_len, uint8_t *level)
{
	ZSTD_DCtx *dctx;
	size_t result;
	int16_t zstd_level;
	uint32_t c_len;
	const zfs_zstdhdr_t *hdr;
	zfs_zstdhdr_t hdr_copy;

	hdr = (const zfs_zstdhdr_t *)s_start;
	c_len = BE_32(hdr->c_len);

	/*
	 * Make a copy instead of directly converting the header, since we must
	 * not modify the original data that may be used again later.
	 */
	hdr_copy.raw_version_level = BE_32(hdr->raw_version_level);
	uint8_t curlevel = zfs_get_hdrlevel(&hdr_copy);

	/*
	 * NOTE: We ignore the ZSTD version for now. As soon as any
	 * incompatibility occurs, it has to be handled accordingly.
	 * The version can be accessed via `hdr_copy.version`.
	 */

	/*
	 * Convert and check the level
	 * An invalid level is a strong indicator for data corruption! In such
	 * case return an error so the upper layers can try to fix it.
	 */
	if (unlikely(zstd_enum_to_level(curlevel, &zstd_level))) {
		ZSTDSTAT_BUMP(zstd_stat_dec_inval);
		return (1);
	}

	ASSERT3U(d_len, >=, s_len);
	ASSERT3U(curlevel, !=, ZIO_COMPLEVEL_INHERIT);

	/* Invalid compressed buffer size encoded at start */
	if (unlikely(c_len + sizeof (*hdr) > s_len)) {
		ZSTDSTAT_BUMP(zstd_stat_dec_header_inval);
		return (1);
	}

	dctx = obj_grab(&dctx_pool);
	ASSERT3P(dctx, !=, NULL);
	if (unlikely(!dctx)) {
		/*
		 * really shouldn't happen - dctx allocations can't fail
		 * - but we'll defend against it anyway.
		 */
		ZSTDSTAT_BUMP(zstd_stat_dec_alloc_fail);
		return (1);
	}

	/* Set header type to "magicless" */
	ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless);

	/* Decompress the data and release the context */
	result = ZSTD_decompressDCtx(dctx, d_start, d_len, hdr->data, c_len);

	/*
	 * Returns 0 on success (decompression function returned non-negative)
	 * and non-zero on failure (decompression function returned negative.
	 */
	if (unlikely(ZSTD_isError(result))) {
		ZSTDSTAT_BUMP(zstd_stat_dec_fail);

		(void) ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);

		obj_ungrab(&dctx_pool, dctx);
		return (1);
	}

	obj_ungrab(&dctx_pool, dctx);

	if (level) {
		*level = curlevel;
	}

	return (0);
}

/* Decompress datablock using zstd */
int
zfs_zstd_decompress(void *s_start, void *d_start, size_t s_len, size_t d_len,
    int level __maybe_unused)
{
	return (zfs_zstd_decompress_level(s_start, d_start, s_len, d_len,
	    NULL));
}

/* supplied as custom allocator() to zstd code */
static void *
zstd_alloc_cb(void *opaque __maybe_unused, size_t size)
{
	int const try_harder = (opaque != NULL);
	struct zstd_kmem_hdr *z;
	size_t nbytes = sizeof (*z) + size;

	z = vmem_alloc(nbytes, KM_NOSLEEP);
	if (!z) {
		ZSTDSTAT_BUMP(zstd_stat_alloc_fail);
		if (try_harder) {
			/* Try harder, KM_SLEEP implies that it can't fail */
			z = vmem_alloc(nbytes, KM_SLEEP);
		} else {
			return (NULL);
		}
	}

	z->kmem_size = nbytes;
	return ((void*)z->data);
}

/* supplied as custom free() to zstd code */
static void
zstd_free_cb(void *opaque __maybe_unused, void *ptr)
{
	ASSERT3P(ptr, !=, NULL);
	struct zstd_kmem_hdr *z = (ptr - sizeof (struct zstd_kmem_hdr));
	vmem_free(z, z->kmem_size);
}

/* Initialize zstd-related memory handling */
static int __init
zstd_mem_init(void)
{
	objpool_init(&cctx_pool);
	objpool_init(&dctx_pool);
	return (0);
}

/* Release zstd-related memory handling */
static void __exit
zstd_mem_deinit(void)
{
	objpool_destroy(&cctx_pool);
	objpool_destroy(&dctx_pool);
}

/* release unused memory from pools */
void
zfs_zstd_cache_reap_now(void)
{
	objpool_reap(&cctx_pool);
	objpool_reap(&dctx_pool);
}

extern int __init
zstd_init(void)
{
	zstd_mem_init();

	/* Initialize kstat */
	zstd_ksp = kstat_create("zfs", 0, "zstd", "misc",
	    KSTAT_TYPE_NAMED, sizeof (zstd_stats) / sizeof (kstat_named_t),
	    KSTAT_FLAG_VIRTUAL);
	if (zstd_ksp != NULL) {
		zstd_ksp->ks_data = &zstd_stats;
		kstat_install(zstd_ksp);
#ifdef _KERNEL
		zstd_ksp->ks_update = kstat_zstd_update;
#endif
	}

	return (0);
}

extern void
zstd_fini(void)
{
	/* Deinitialize kstat */
	if (zstd_ksp != NULL) {
		kstat_delete(zstd_ksp);
		zstd_ksp = NULL;
	}

	/* Deinit memory pools */
	zstd_mem_deinit();
}

#if defined(_KERNEL)
#ifdef __FreeBSD__
module_init(zstd_init);
module_exit(zstd_fini);
#endif

ZFS_MODULE_PARAM(zfs, zstd_, lz4_pass, INT, ZMOD_RW,
	"Try an LZ4 pass to determine whether we should early abort.");
ZFS_MODULE_PARAM(zfs, zstd_, zstd_pass, INT, ZMOD_RW,
	"Try a ZSTD pass after LZ4 to determine whether we should early abort.");
ZFS_MODULE_PARAM(zfs, zstd_, firstpass_mode, INT, ZMOD_RW,
	"If trying zstd after LZ4, what config to try.");
ZFS_MODULE_PARAM(zfs, zstd_, cutoff_level, INT, ZMOD_RW,
	"If trying zstd after LZ4, what level to only try it above (e.g. a value of 3 means 4+ will have it tried)");
ZFS_MODULE_PARAM(zfs, zstd_, abort_size, UINT, ZMOD_RW,
	"Only >= this size should we try early abort at all");
ZFS_MODULE_PARAM(zfs, zstd_, hard_moed, UINT, ZMOD_RW,
	"Ignore the rules; 1 == all the compression passes on eligible blocks, >=2 all the compression passes always");
//ZFS_MODULE_PARAM(zfs, zstd_, zstd_shift_size, UINT, ZMOD_RW,
//	"How many bits to offset the d_len given to ZSTD-1");
ZFS_MODULE_PARAM(zfs, zstd_, ea_division_mode, UINT, ZMOD_RW,
	"Do we scale zstd_abort_size");
ZFS_MODULE_PARAM(zfs, zstd_, ea_divisor, UINT, ZMOD_RW,
	"How fast do we scale down the cutoff");
ZFS_MODULE_PARAM(zfs, zstd_, ea_level_factor, UINT, ZMOD_RW,
	"How often do we change increments");
ZFS_MODULE_PARAM(zfs, zstd_, lz4_shift_size, UINT, ZMOD_RW,
	"How many bits to offset the d_len given to LZ4");

#endif
