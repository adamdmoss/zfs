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
#include "lib/zstd_errors.h"


#if defined(__KERNEL__)
#if 0
extern	int printk(const char *fmt, ...);
#define aprint printk
#else
#define aprint(...) do{}while(0)
#endif
#else
#define aprint(...) printf(__VA_ARGS__)
#endif


kstat_t *zstd_ksp = NULL;

typedef struct zstd_stats {
	kstat_named_t	zstd_stat_alloc_fail;
	kstat_named_t	zstd_stat_alloc_fallback;
	kstat_named_t	zstd_stat_com_alloc_fail;
	kstat_named_t	zstd_stat_dec_alloc_fail;
	kstat_named_t	zstd_stat_com_inval;
	kstat_named_t	zstd_stat_dec_inval;
	kstat_named_t	zstd_stat_dec_header_inval;
	kstat_named_t	zstd_stat_com_fail;
	kstat_named_t	zstd_stat_dec_fail;
	kstat_named_t	zstd_stat_buffers;
	kstat_named_t	zstd_stat_size;
} zstd_stats_t;

static zstd_stats_t zstd_stats = {
	{ "alloc_fail",			KSTAT_DATA_UINT64 },
	{ "alloc_fallback",		KSTAT_DATA_UINT64 },
	{ "compress_alloc_fail",	KSTAT_DATA_UINT64 },
	{ "decompress_alloc_fail",	KSTAT_DATA_UINT64 },
	{ "compress_level_invalid",	KSTAT_DATA_UINT64 },
	{ "decompress_level_invalid",	KSTAT_DATA_UINT64 },
	{ "decompress_header_invalid",	KSTAT_DATA_UINT64 },
	{ "compress_failed",		KSTAT_DATA_UINT64 },
	{ "decompress_failed",		KSTAT_DATA_UINT64 },
	{ "buffers",			KSTAT_DATA_UINT64 },
	{ "size",			KSTAT_DATA_UINT64 },
};

/* Enums describing the allocator type specified by kmem_type in zstd_kmem */
enum zstd_kmem_type {
	ZSTD_KMEM_UNKNOWN = 0,
	/* Allocation type using kmem_vmalloc */
	ZSTD_KMEM_DEFAULT,
	/* Pool based allocation using mempool_alloc */
	ZSTD_KMEM_POOL,
	/* Reserved fallback memory for decompression only */
	ZSTD_KMEM_DCTX,
	ZSTD_KMEM_COUNT,
};

/* Structure for pooled memory objects */
struct zstd_pool {
	void *mem;
	size_t size;
	kmutex_t barrier;
	hrtime_t timeout;
};

/* Global structure for handling memory allocations */
struct zstd_kmem {
	enum zstd_kmem_type kmem_type;
	size_t kmem_size;
	struct zstd_pool *pool;
};

/* Fallback memory structure used for decompression only if memory runs out */
struct zstd_fallback_mem {
	size_t mem_size;
	void *mem;
	kmutex_t barrier;
};

struct zstd_levelmap {
	int16_t zstd_level;
	enum zio_zstd_levels level;
};

/*
 * ZSTD memory handlers
 *
 * For decompression we use a different handler which also provides fallback
 * memory allocation in case memory runs out.
 *
 * The ZSTD handlers were split up for the most simplified implementation.
 */
static void *zstd_alloc(void *opaque, size_t size);
static void *zstd_dctx_alloc(void *opaque, size_t size);
static void zstd_free(void *opaque, void *ptr);

/* Compression memory handler */
static const ZSTD_customMem zstd_malloc = {
	zstd_alloc,
	zstd_free,
	NULL,
};

/* Decompression memory handler */
static const ZSTD_customMem zstd_dctx_malloc = {
	zstd_dctx_alloc,
	zstd_free,
	NULL,
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

/*
 * This variable represents the maximum count of the pool based on the number
 * of CPUs plus some buffer. We default to cpu count * 4, see init_zstd.
 */
static int pool_count = 16;

#define	ZSTD_POOL_MAX		pool_count
#define	ZSTD_POOL_TIMEOUT	60 * 2

static struct zstd_fallback_mem zstd_dctx_fallback;
static struct zstd_pool *zstd_mempool_cctx;
static struct zstd_pool *zstd_mempool_dctx;


static void
zstd_mempool_reap(struct zstd_pool *zstd_mempool)
{
	struct zstd_pool *pool;

	if (!zstd_mempool || !ZSTDSTAT(zstd_stat_buffers)) {
		return;
	}

	/* free obsolete slots */
	for (int i = 0; i < ZSTD_POOL_MAX; i++) {
		pool = &zstd_mempool[i];
		if (pool->mem && mutex_tryenter(&pool->barrier)) {
			/* Free memory if unused object older than 2 minutes */
			if (pool->mem && gethrestime_sec() > pool->timeout) {
				vmem_free(pool->mem, pool->size);
				ZSTDSTAT_SUB(zstd_stat_buffers, 1);
				ZSTDSTAT_SUB(zstd_stat_size, pool->size);
				pool->mem = NULL;
				pool->size = 0;
				pool->timeout = 0;
			}
			mutex_exit(&pool->barrier);
		}
	}
}

/*
 * Try to get a cached allocated buffer from memory pool or allocate a new one
 * if necessary. If a object is older than 2 minutes and does not fit the
 * requested size, it will be released and a new cached entry will be allocated.
 * If other pooled objects are detected without being used for 2 minutes, they
 * will be released, too.
 *
 * The concept is that high frequency memory allocations of bigger objects are
 * expensive. So if a lot of work is going on, allocations will be kept for a
 * while and can be reused in that time frame.
 *
 * The scheduled release will be updated every time a object is reused.
 */

static void *
zstd_mempool_alloc(struct zstd_pool *zstd_mempool, size_t size)
{
	struct zstd_pool *pool;
	struct zstd_kmem *mem = NULL;

	if (!zstd_mempool) {
		return (NULL);
	}

	/* Seek for preallocated memory slot and free obsolete slots */
	for (int i = 0; i < ZSTD_POOL_MAX; i++) {
		pool = &zstd_mempool[i];
		/*
		 * This lock is simply a marker for a pool object beeing in use.
		 * If it's already hold, it will be skipped.
		 *
		 * We need to create it before checking it to avoid race
		 * conditions caused by running in a threaded context.
		 *
		 * The lock is later released by zstd_mempool_free.
		 */
		if (mutex_tryenter(&pool->barrier)) {
			/*
			 * Check if objects fits the size, if so we take it and
			 * update the timestamp.
			 */
			if (pool->mem && size <= pool->size) {
				pool->timeout = gethrestime_sec() +
				    ZSTD_POOL_TIMEOUT;
				mem = pool->mem;
				return (mem);
			}
			mutex_exit(&pool->barrier);
		}
	}

	/*
	 * If no preallocated slot was found, try to fill in a new one.
	 *
	 * We run a similar algorithm twice here to avoid pool fragmentation.
	 * The first one may generate holes in the list if objects get released.
	 * We always make sure that these holes get filled instead of adding new
	 * allocations constantly at the end.
	 */
	for (int i = 0; i < ZSTD_POOL_MAX; i++) {
		pool = &zstd_mempool[i];
		if (mutex_tryenter(&pool->barrier)) {
			/* Object is free, try to allocate new one */
			if (!pool->mem) {
				mem = vmem_alloc(size, KM_SLEEP);
				if (mem) {
					ZSTDSTAT_ADD(zstd_stat_buffers, 1);
					ZSTDSTAT_ADD(zstd_stat_size, size);
					pool->mem = mem;
					pool->size = size;
					/* Keep track for later release */
					mem->pool = pool;
					mem->kmem_type = ZSTD_KMEM_POOL;
					mem->kmem_size = size;
				}
			}

			if (size <= pool->size) {
				/* Update timestamp */
				pool->timeout = gethrestime_sec() +
				    ZSTD_POOL_TIMEOUT;

				return (pool->mem);
			}

			mutex_exit(&pool->barrier);
		}
	}

	/*
	 * If the pool is full or the allocation failed, try lazy allocation
	 * instead.
	 */
	if (!mem) {
		mem = vmem_alloc(size, KM_NOSLEEP);
		if (mem) {
			mem->pool = NULL;
			mem->kmem_type = ZSTD_KMEM_DEFAULT;
			mem->kmem_size = size;
		}
	}

	return (mem);
}

/* Mark object as released by releasing the barrier mutex */
static void
zstd_mempool_free(struct zstd_kmem *z)
{
	mutex_exit(&z->pool->barrier);
}

/////////////////////////////////////////// OBJECT POOLING UTILS
////////////////////////////////////////////////////////////////
#define GRABAMP 0*10000 /*>0 to amplify grab/ungrab contention for testing*/
#define OBJPOOL_TIMEOUT_SEC 15
#define ZSPINLOCK_TRYLOCK(L) (!atomic_cas_32((L), 0U, 1U))
#define ZSPINLOCK_LOCK(L) while(!ZSPINLOCK_TRYLOCK(L)){ cond_resched(); }
#define ZSPINLOCK_UNLOCK(L) atomic_dec_32(L)
#define ZSPINLOCK_INIT(L) (*(L) = 0)
#define ZSPINLOCK_DESTROY(L) /* don't need to do anything for this */
typedef uint32_t zyieldingspinlock_t;

typedef struct {
	zyieldingspinlock_t listlock;
	int count;
	void **list;

	int64_t last_accessed_jiffy; /* for idle-reap */

	void*(*obj_alloc)(void);
	void(*obj_free)(void*);
	void(*obj_reset)(void*);
	const char*const pool_name;
} objpool_t;

static void objpool_init(objpool_t *objpool);
static void objpool_reap(objpool_t *objpool);
static void objpool_destroy(objpool_t *objpool);

static void* obj_grab(objpool_t *objpool);
static void  obj_ungrab(objpool_t *objpool, void* obj);

static void
objpool_reset_idle_timer(objpool_t *objpool)
{
	const int64_t now_jiffy = ddi_get_lbolt64();
	objpool->last_accessed_jiffy = now_jiffy;
}

static void
objpool_init(objpool_t *objpool)
{
	ZSPINLOCK_INIT(&objpool->listlock);

	objpool->count = 0;
	objpool->list = NULL;
	objpool_reset_idle_timer(objpool);
}

static void
objpool_clearunused(objpool_t *objpool)
{
	ZSPINLOCK_LOCK(&objpool->listlock);
	for (int i = 0; i < objpool->count; ++i)
	{
		if (NULL == objpool->list[i])
		{
			// if ANY object is still in use then don't do anything
			ZSPINLOCK_UNLOCK(&objpool->listlock);
			return;
		}
	}
	for (int i = 0; i < objpool->count; ++i)
	{
		objpool->obj_free(objpool->list[i]);
	}
	if (objpool->count > 0)
	{
		VERIFY3P(objpool->list, !=, NULL);
		kmem_free(objpool->list, sizeof(void *) * objpool->count);
		objpool->list = NULL;
	}
	else
	{
		VERIFY3P(objpool->list, ==, NULL);
	}
	objpool->count = 0;
	ZSPINLOCK_UNLOCK(&objpool->listlock);
	objpool_reset_idle_timer(objpool);
}

static void
objpool_reap(objpool_t *objpool)
{
	int64_t now_jiffy = ddi_get_lbolt64();
	//aprint("(considering idle-reap for pool \"%s\": now=%lld lastused=%lld reaptime=%lld)\n", objpool->pool_name, (long long int)now_jiffy, (long long int)objpool->last_accessed_jiffy, (long long int)(objpool->last_accessed_jiffy + SEC_TO_TICK(OBJPOOL_TIMEOUT_SEC)));
	if (objpool->last_accessed_jiffy > now_jiffy /*wrap*/
	    || now_jiffy - objpool->last_accessed_jiffy
		    > SEC_TO_TICK(OBJPOOL_TIMEOUT_SEC))
	{
		//aprint("actually doing idle-reap for pool \"%s\"\n", objpool->pool_name);
		objpool_clearunused(objpool);
	}
}

static void
objpool_destroy(objpool_t *objpool)
{
	objpool_clearunused(objpool);
	VERIFY3U(objpool->count, ==, 0);
	if (objpool->count == 0)
	{
		ZSPINLOCK_DESTROY(&objpool->listlock);
	}
}

static void*
obj_grab(objpool_t *objpool)
{
	void* found = NULL;
	ZSPINLOCK_LOCK(&objpool->listlock);
	const int threadpid = (int)getpid();
	for (int i = 0; i < objpool->count; ++i)
	{
		int j = (i + threadpid) % objpool->count;
		if ((found = objpool->list[j]) != NULL)
		{
			objpool->list[j] = NULL;
			break;
		}
	}
	if (likely(found!=NULL))
	{
		// slightly subtle optimization; compressor needs to reset *stream* (only on error), we reset *parameters*
		objpool->obj_reset(found);
	}
	else
	{
		//aprint("ADAM: pool \"%s\" growing list from %d to %d entries\n", objpool->pool_name, objpool->count, 1 + objpool->count);
		int newlistbytes = sizeof(void *) * (objpool->count + 1);
		void **newlist = kmem_alloc(newlistbytes, KM_SLEEP);
		if (likely(newlist!=NULL))
		{
			found = objpool->obj_alloc();
			if (likely(found!=NULL))
			{
				newlist[0] = NULL;
				if (likely(objpool->count > 0))
				{
					VERIFY3P(objpool->list, !=, NULL);
					memcpy(&newlist[1], &objpool->list[0], sizeof(void *) * (objpool->count));
					kmem_free(objpool->list, sizeof(void *) * (objpool->count));
				}
				else
				{
					VERIFY3P(objpool->list, ==, NULL);
				}
				objpool->list = newlist;
				++objpool->count;
				// total success
				//aprint("ADAM: expanded lists and grabbed new entry %p\n", found);
			}
			else
			{
				kmem_free(newlist, newlistbytes);
				aprint("ADAM: failed to alloc new %s for list\n", objpool->pool_name);
			}
		}
		else
		{
			aprint("ADAM: failed to alloc larger list\n");
		}
		//aprint("ADAM: resulting %s is %p\n", objpool->pool_name, found);
	}
	ZSPINLOCK_UNLOCK(&objpool->listlock);
	return found;
}

static void
obj_ungrab(objpool_t *objpool, void* obj)
{
	VERIFY3P(obj, !=, NULL);
	ZSPINLOCK_LOCK(&objpool->listlock);
	const int threadpid = (int)getpid();
	for (int i = 0; i < objpool->count; ++i)
	{
		int j = (i + threadpid) % objpool->count;
		if (objpool->list[j] == NULL)
		{
			objpool->list[j] = obj;
			break;
		}
	}
	ZSPINLOCK_UNLOCK(&objpool->listlock);
	objpool_reset_idle_timer(objpool);
}

/////////////////////////////////////////// OBJECT POOLS
////////////////////////////////////////////////////////

static void *
cctx_alloc(void)
{
	return ZSTD_createCCtx_advanced(zstd_malloc);
}
static void
cctx_free(void *ptr)
{
	ZSTD_freeCCtx(ptr);
}
static void
cctx_reset(void *ptr)
{
	ZSTD_CCtx_reset(ptr, ZSTD_reset_parameters);
}

static void *
dctx_alloc(void)
{
	return ZSTD_createDCtx_advanced(zstd_dctx_malloc);
}
static void
dctx_free(void *ptr)
{
	ZSTD_freeDCtx(ptr);
}
static void
dctx_reset(void *ptr)
{
	ZSTD_DCtx_reset(ptr, ZSTD_reset_parameters);
}

static objpool_t cctx_pool = {
	.obj_alloc = cctx_alloc,
	.obj_free = cctx_free,
	.obj_reset = cctx_reset,
	.pool_name = "zstdcctx"
	};

static objpool_t dctx_pool = {
	.obj_alloc = dctx_alloc,
	.obj_free = dctx_free,
	.obj_reset = dctx_reset,
	.pool_name = "zstddctx"
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
	if (zstd_enum_to_level(level, &zstd_level)) {
		ZSTDSTAT_BUMP(zstd_stat_com_inval);
		return (s_len);
	}

	ASSERT3U(d_len, >=, sizeof (*hdr));
	ASSERT3U(d_len, <=, s_len);
	ASSERT3U(zstd_level, !=, 0);
	for (int i=0; i<GRABAMP; ++i) { void* foo = obj_grab(&cctx_pool); if(foo) obj_ungrab(&cctx_pool, foo);}
	cctx = obj_grab(&cctx_pool);

	/*
	 * Out of kernel memory, gently fall through - this will disable
	 * compression in zio_compress_data
	 */
	if (!cctx) {
		ZSTDSTAT_BUMP(zstd_stat_com_alloc_fail);
		return (s_len);
	}

	/* Set the compression level */
	ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, zstd_level);

	/* Use the "magicless" zstd header which saves us 4 header bytes */
	ZSTD_CCtx_setParameter(cctx, ZSTD_c_format, ZSTD_f_zstd1_magicless);

	/*
	 * Disable redundant checksum calculation and content size storage since
	 * this is already done by ZFS itself.
	 */
	ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 0);
	ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0);

	const size_t MAX_INPUT_GULP = 1;//4095;
	const size_t MAX_OUTPUT_GULP = 3333;
	size_t src_remain = s_len;
	size_t dest_remain = d_len - sizeof(*hdr);
	char *src_ptr = s_start;
	char *dest_ptr = hdr->data;
	aprint("starting new record... (src_remain=%ld dest_remain=%ld)\n", src_remain, dest_remain);
	size_t compressedSize = 0;// /*hack*/ (size_t)-ZSTD_error_GENERIC;
	int need_more_writespace = 0;
	{
		for(;;)
		{
			int is_final_input_gulp = 0;
			int is_final_output_gulp = 0;
			//int gotta_continue = 1;
			size_t this_output_gulp_size = MAX_OUTPUT_GULP;
			size_t this_input_gulp_size = MAX_INPUT_GULP;
			if (src_remain <= this_input_gulp_size)
			{
				this_input_gulp_size = src_remain;
				is_final_input_gulp = 1;
			}
			if (dest_remain <= this_output_gulp_size)
			{
				this_output_gulp_size = dest_remain;
				is_final_output_gulp = 1;
			}
			//gotta_continue = (need_more_writespace || !is_final_input_gulp);
			ZSTD_EndDirective endtype = ZSTD_e_continue;
			if (need_more_writespace) endtype = ZSTD_e_flush;
			else if (is_final_input_gulp) endtype = ZSTD_e_end;
			aprint("gulp: ingulpsize=%ld, outgulpsize=%ld, src_remain=%ld, dest_remain=%ld, ENDTYPE->%d\n",
			this_input_gulp_size, this_output_gulp_size, src_remain, dest_remain, (int)endtype);
			VERIFY(src_remain > 0 || dest_remain > 0);
			VERIFY(this_input_gulp_size > 0 || this_output_gulp_size > 0);
			ZSTD_outBuffer outBuff = {dest_ptr, this_output_gulp_size, 0};
			ZSTD_inBuffer inBuff = {src_ptr, this_input_gulp_size, 0};

			size_t status = ZSTD_compressStream2(cctx, &outBuff, &inBuff, endtype);

			if (ZSTD_isError(status))
			{
				compressedSize = status;
				aprint("status was error: %s\n", ZSTD_getErrorName(status));

				ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
				goto badc;
			}

			need_more_writespace = (status != 0);

			if (is_final_output_gulp &&
			    outBuff.pos == outBuff.size) // what about the case where we *exactly* fit into the output buffer, that's not really an overflow is it...?  can/should we check if input is all consumed here?
			{
				compressedSize = /*hacky fake error*/ (size_t)-ZSTD_error_dstSize_tooSmall;
				aprint("FULL(output full, input remains); outpos==outsize\n");

				ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
				goto badc; // ?
			}

			compressedSize += outBuff.pos;
			
			dest_ptr += outBuff.pos;
			VERIFY3S(dest_remain, >=, outBuff.pos);
			dest_remain -= outBuff.pos;
			if (dest_remain == 0) // what about the case where we *exactly* fit into the output buffer, that's not really an overflow is it...?  can/should we check if input is all consumed here?
			{
				compressedSize = /*hacky fake error*/ (size_t)-ZSTD_error_dstSize_tooSmall;
				aprint("FULL(output full, input remains); dest_remain == 0, compressed_size=%ld\n", compressedSize);

				ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
				goto badc; // ?
			}

			src_ptr += inBuff.pos;
			VERIFY3S(src_remain, >=, inBuff.pos);
			src_remain -= inBuff.pos;
			if (src_remain == 0)
			{
				if (!need_more_writespace)
				{
					if (endtype == ZSTD_e_end)
					{
						aprint("END(src_remain == 0, compressed_size=%ld, outbuff.pos<outbuff.size)\n", compressedSize);
						// totally done
						break;
					}
					else
					{
						aprint("no input remaining, don't need more write-space, going around again to write epilogue I guess (compressedsize=%ld)\n", compressedSize);
					}
				}
				else
				{
					aprint("input consumed but still need more output space... (%ld internal bytes to drain)\n", status);
				}
			}
#ifdef __KERNEL__
			//kpreempt();
#else
#endif
			cond_resched(); // possibly yield before taking next gulp
		}
	}
badc:

	aprint("compressedSize: %zu (iserr?%d - %s)", compressedSize, ZSTD_isError(compressedSize), ZSTD_getErrorName(compressedSize));
	c_len = compressedSize;

	obj_ungrab(&cctx_pool, cctx);

	/* Error in the compression routine, disable compression. */
	if (ZSTD_isError(c_len)) {
		/*
		 * If we are aborting the compression because the saves are
		 * too small, that is not a failure. Everything else is a
		 * failure, so increment the compression failure counter.
		 */
		if (ZSTD_getErrorCode(c_len) != ZSTD_error_dstSize_tooSmall)
		{
			aprint("ERROR status... ending\n");
			ZSTDSTAT_BUMP(zstd_stat_com_fail);
		}
		else
		{
			aprint("(dest was too small?)... ending\n");
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
	hdr->version = ZSTD_VERSION_NUMBER;
	hdr->level = level;
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

	/*
	 * NOTE: We ignore the ZSTD version for now. As soon as any
	 * incompatibility occurrs, it has to be handled accordingly.
	 * The version can be accessed via `hdr_copy.version`.
	 */

	/*
	 * Convert and check the level
	 * An invalid level is a strong indicator for data corruption! In such
	 * case return an error so the upper layers can try to fix it.
	 */
	if (zstd_enum_to_level(hdr_copy.level, &zstd_level)) {
		ZSTDSTAT_BUMP(zstd_stat_dec_inval);
		return (1);
	}

	ASSERT3U(d_len, >=, s_len);
	ASSERT3U(hdr_copy.level, !=, ZIO_COMPLEVEL_INHERIT);

	/* Invalid compressed buffer size encoded at start */
	if (c_len + sizeof (*hdr) > s_len) {
		ZSTDSTAT_BUMP(zstd_stat_dec_header_inval);
		return (1);
	}

	for (int i=0; i<GRABAMP; ++i) { void* foo = obj_grab(&dctx_pool); if(foo) obj_ungrab(&dctx_pool, foo);}
	dctx = obj_grab(&dctx_pool);
	if (!dctx) {
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
	if (ZSTD_isError(result)) {
		ZSTDSTAT_BUMP(zstd_stat_dec_fail);
		ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
		obj_ungrab(&dctx_pool, dctx);
		return (1);
	}

	obj_ungrab(&dctx_pool, dctx);

	if (level) {
		*level = hdr_copy.level;
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

/* Allocator for zstd compression context using mempool_allocator */
static void *
zstd_alloc(void *opaque __maybe_unused, size_t size)
{
	size_t nbytes = sizeof (struct zstd_kmem) + size;
	struct zstd_kmem *z = NULL;

	z = (struct zstd_kmem *)zstd_mempool_alloc(zstd_mempool_cctx, nbytes);

	if (!z) {
		ZSTDSTAT_BUMP(zstd_stat_alloc_fail);
		return (NULL);
	}

	return ((void*)z + (sizeof (struct zstd_kmem)));
}

/*
 * Allocator for zstd decompression context using mempool_allocator with
 * fallback to reserved memory if allocation fails
 */
static void *
zstd_dctx_alloc(void *opaque __maybe_unused, size_t size)
{
	size_t nbytes = sizeof (struct zstd_kmem) + size;
	struct zstd_kmem *z = NULL;
	enum zstd_kmem_type type = ZSTD_KMEM_DEFAULT;

	z = (struct zstd_kmem *)zstd_mempool_alloc(zstd_mempool_dctx, nbytes);
	if (!z) {
		/* Try harder, decompression shall not fail */
		z = vmem_alloc(nbytes, KM_SLEEP);
		if (z) {
			z->pool = NULL;
		}
		ZSTDSTAT_BUMP(zstd_stat_alloc_fail);
	} else {
		return ((void*)z + (sizeof (struct zstd_kmem)));
	}

	/* Fallback if everything fails */
	if (!z) {
		/*
		 * Barrier since we only can handle it in a single thread. All
		 * other following threads need to wait here until decompression
		 * is completed. zstd_free will release this barrier later.
		 */
		mutex_enter(&zstd_dctx_fallback.barrier);

		z = zstd_dctx_fallback.mem;
		type = ZSTD_KMEM_DCTX;
		ZSTDSTAT_BUMP(zstd_stat_alloc_fallback);
	}

	/* Allocation should always be successful */
	if (!z) {
		return (NULL);
	}

	z->kmem_type = type;
	z->kmem_size = nbytes;

	return ((void*)z + (sizeof (struct zstd_kmem)));
}

/* Free allocated memory by its specific type */
static void
zstd_free(void *opaque __maybe_unused, void *ptr)
{
	struct zstd_kmem *z = (ptr - sizeof (struct zstd_kmem));
	enum zstd_kmem_type type;

	ASSERT3U(z->kmem_type, <, ZSTD_KMEM_COUNT);
	ASSERT3U(z->kmem_type, >, ZSTD_KMEM_UNKNOWN);

	type = z->kmem_type;
	switch (type) {
	case ZSTD_KMEM_DEFAULT:
		vmem_free(z, z->kmem_size);
		break;
	case ZSTD_KMEM_POOL:
		zstd_mempool_free(z);
		break;
	case ZSTD_KMEM_DCTX:
		mutex_exit(&zstd_dctx_fallback.barrier);
		break;
	default:
		break;
	}
}

/* Allocate fallback memory to ensure safe decompression */
static void __init
create_fallback_mem(struct zstd_fallback_mem *mem, size_t size)
{
	mem->mem_size = size;
	mem->mem = vmem_zalloc(mem->mem_size, KM_SLEEP);
	mutex_init(&mem->barrier, NULL, MUTEX_DEFAULT, NULL);
}

/* Initialize memory pool barrier mutexes */
static void __init
zstd_mempool_init(void)
{
	objpool_init(&cctx_pool);
	objpool_init(&dctx_pool);

	// OTHER STUFF...
	zstd_mempool_cctx = (struct zstd_pool *)
	    kmem_zalloc(ZSTD_POOL_MAX * sizeof (struct zstd_pool), KM_SLEEP);
	zstd_mempool_dctx = (struct zstd_pool *)
	    kmem_zalloc(ZSTD_POOL_MAX * sizeof (struct zstd_pool), KM_SLEEP);

	for (int i = 0; i < ZSTD_POOL_MAX; i++) {
		mutex_init(&zstd_mempool_cctx[i].barrier, NULL,
		    MUTEX_DEFAULT, NULL);
		mutex_init(&zstd_mempool_dctx[i].barrier, NULL,
		    MUTEX_DEFAULT, NULL);
	}
}

/* Initialize zstd-related memory handling */
static int __init
zstd_meminit(void)
{
	zstd_mempool_init();

	/*
	 * Estimate the size of the fallback decompression context.
	 * The expected size on x64 with current ZSTD should be about 160 KB.
	 */
	create_fallback_mem(&zstd_dctx_fallback,
	    P2ROUNDUP(ZSTD_estimateDCtxSize() + sizeof (struct zstd_kmem),
	    PAGESIZE));

	return (0);
}

/* Release object from pool and free memory */
static void __exit
release_pool(struct zstd_pool *pool)
{
	mutex_destroy(&pool->barrier);
	vmem_free(pool->mem, pool->size);
	pool->mem = NULL;
	pool->size = 0;
}

/* Release memory pool objects */
static void __exit
zstd_mempool_deinit(void)
{
	/* must release these object pools before releasing the mempools
	    below, since these use the mempools */
	objpool_destroy(&cctx_pool);
	objpool_destroy(&dctx_pool);

	for (int i = 0; i < ZSTD_POOL_MAX; i++) {
		release_pool(&zstd_mempool_cctx[i]);
		release_pool(&zstd_mempool_dctx[i]);
	}

	kmem_free(zstd_mempool_dctx, ZSTD_POOL_MAX * sizeof (struct zstd_pool));
	kmem_free(zstd_mempool_cctx, ZSTD_POOL_MAX * sizeof (struct zstd_pool));
	zstd_mempool_dctx = NULL;
	zstd_mempool_cctx = NULL;
}

/* release unused memory from pool */

void
zfs_zstd_cache_reap_now(void)
{
	objpool_reap(&cctx_pool);
	objpool_reap(&dctx_pool);
	/*
	 * calling alloc with zero size seeks
	 * and releases old unused objects
	 */
	zstd_mempool_reap(zstd_mempool_cctx);
	zstd_mempool_reap(zstd_mempool_dctx);
}

extern int __init
zstd_init(void)
{
	/* Set pool size by using maximum sane thread count * 4 */
	pool_count = (boot_ncpus * 4);
	zstd_meminit();

	/* Initialize kstat */
	zstd_ksp = kstat_create("zfs", 0, "zstd", "misc",
	    KSTAT_TYPE_NAMED, sizeof (zstd_stats) / sizeof (kstat_named_t),
	    KSTAT_FLAG_VIRTUAL);
	if (zstd_ksp != NULL) {
		zstd_ksp->ks_data = &zstd_stats;
		kstat_install(zstd_ksp);
	}

	return (0);
}

extern void __exit
zstd_fini(void)
{
	/* Deinitialize kstat */
	if (zstd_ksp != NULL) {
		kstat_delete(zstd_ksp);
		zstd_ksp = NULL;
	}

	/* Release fallback memory */
	vmem_free(zstd_dctx_fallback.mem, zstd_dctx_fallback.mem_size);
	mutex_destroy(&zstd_dctx_fallback.barrier);

	/* Deinit memory pool */
	zstd_mempool_deinit();
}

#if defined(_KERNEL)
module_init(zstd_init);
module_exit(zstd_fini);

ZFS_MODULE_DESCRIPTION("ZSTD Compression for ZFS");
ZFS_MODULE_LICENSE("Dual BSD/GPL");
ZFS_MODULE_VERSION(ZSTD_VERSION_STRING);

EXPORT_SYMBOL(zfs_zstd_compress);
EXPORT_SYMBOL(zfs_zstd_decompress_level);
EXPORT_SYMBOL(zfs_zstd_decompress);
EXPORT_SYMBOL(zfs_zstd_cache_reap_now);
#endif
