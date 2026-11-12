/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * base/arcr_threadpool.h - Cross-Platform Hardware Thread Pool & Parallel For-Loop
 *
 * Maximizes multi-core CPU hardware utilization for 64x64 tile rasterization
 * while incurring 0 thread-creation overhead on single-core vintage CPUs.
 */
#ifndef ARCR_THREADPOOL_H
#define ARCR_THREADPOOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ArcrThreadPool ArcrThreadPool;

typedef void (*ArcrParallelTaskFn)(void *userdata, uint32_t item_index, uint32_t worker_id);

/* Queries logical hardware CPU cores (1..64) */
uint32_t arcr_hardware_cpu_threads(void);

/*
 * Creates a persistent worker pool.
 * If `desired_threads == 0`, auto-scales to hardware CPU threads (clamped to 1..16).
 * If resolved thread count == 1, executes inline with zero OS thread allocation!
 */
ArcrThreadPool *arcr_threadpool_create(uint32_t desired_threads);
void            arcr_threadpool_destroy(ArcrThreadPool *pool);

uint32_t arcr_threadpool_worker_count(const ArcrThreadPool *pool);

/*
 * Executes `fn(userdata, idx, worker_id)` for `idx` in `[0, item_count)` across all
 * hardware worker threads using atomic index stealing, blocking until all items complete.
 */
void arcr_threadpool_parallel_for(ArcrThreadPool *pool, uint32_t item_count,
                                  ArcrParallelTaskFn fn, void *userdata);

#endif /* ARCR_THREADPOOL_H */
