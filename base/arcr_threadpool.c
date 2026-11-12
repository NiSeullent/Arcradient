/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * base/arcr_threadpool.c - Hardware Core Detection & Atomic Work-Stealing Thread Pool
 *
 * - Windows: Win2000 SP4+ compatible (GetSystemInfo, CreateThread, Event, InterlockedIncrement)
 * - POSIX:   Linux/macOS pthread + C11 / GCC atomic work stealing
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "arcr_threadpool.h"
#include "arcr_base.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WINVER
#    define WINVER 0x0500
#  endif
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0500
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <process.h>

#  ifndef STACK_SIZE_PARAM_IS_A_RESERVATION
#    define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000u
#  endif

#  define ARCR_WORKER_STACK_BYTES (2u * 1024u * 1024u)

uint32_t arcr_hardware_cpu_threads(void) {
    SYSTEM_INFO si;
    ZeroMemory(&si, sizeof(si));
    GetSystemInfo(&si);
    if (si.dwNumberOfProcessors < 1u) return 1u;
    if (si.dwNumberOfProcessors > 64u) return 64u;
    return (uint32_t)si.dwNumberOfProcessors;
}

typedef struct WorkerArg {
    struct ArcrThreadPool *pool;
    uint32_t               worker_id;
} WorkerArg;

struct ArcrThreadPool {
    uint32_t           total_workers; /* Includes caller thread (worker 0) */
    uint32_t           bg_count;      /* Successfully spawned background workers */
    HANDLE             threads[16];
    WorkerArg          args[16];
    CRITICAL_SECTION   cs;
    HANDLE             wake_event;    /* Manual-reset event signaled while batch active */
    HANDLE             done_event;    /* Auto-reset event signaled when last worker finishes */
    volatile LONG      shutting_down;
    uint64_t           generation;
    volatile LONG      next_index;
    volatile LONG      active_bg;
    uint32_t           item_count;
    ArcrParallelTaskFn task_fn;
    void              *task_ud;
};

static void run_steal_loop(ArcrThreadPool *pool, uint32_t worker_id) {
    for (;;) {
        LONG idx = InterlockedIncrement(&pool->next_index) - 1L;
        if ((uint32_t)idx >= pool->item_count) break;
        pool->task_fn(pool->task_ud, (uint32_t)idx, worker_id);
    }
}

static unsigned __stdcall win32_worker_proc(void *param) {
    WorkerArg      *wa       = (WorkerArg *)param;
    ArcrThreadPool *pool     = wa->pool;
    uint32_t        wid      = wa->worker_id;
    uint64_t        seen_gen = 0u;

    for (;;) {
        EnterCriticalSection(&pool->cs);
        if (pool->shutting_down) {
            LeaveCriticalSection(&pool->cs);
            break;
        }
        if (pool->generation == seen_gen) {
            LeaveCriticalSection(&pool->cs);
            WaitForSingleObject(pool->wake_event, 50);
            continue;
        }
        seen_gen = pool->generation;
        LeaveCriticalSection(&pool->cs);

        run_steal_loop(pool, wid);

        EnterCriticalSection(&pool->cs);
        if (--pool->active_bg == 0L) {
            if (pool->generation == seen_gen && !pool->shutting_down) {
                ResetEvent(pool->wake_event);
            }
            SetEvent(pool->done_event);
        }
        LeaveCriticalSection(&pool->cs);
    }
    return 0u;
}

ArcrThreadPool *arcr_threadpool_create(uint32_t desired_threads) {
    ArcrThreadPool *pool = (ArcrThreadPool *)calloc(1u, sizeof(ArcrThreadPool));
    if (!pool) return NULL;

    uint32_t hw = (desired_threads == 0u) ? arcr_hardware_cpu_threads() : desired_threads;
    hw = ARCR_CLAMP(hw, 1u, 16u);
    uint32_t desired_bg = hw - 1u;
    pool->total_workers = 1u;
    pool->bg_count      = 0u;

    if (desired_bg > 0u) {
        InitializeCriticalSection(&pool->cs);
        pool->wake_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        pool->done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (pool->wake_event && pool->done_event) {
            for (uint32_t i = 0u; i < desired_bg; ++i) {
                uint32_t slot = pool->bg_count;
                pool->args[slot].pool      = pool;
                pool->args[slot].worker_id = slot + 1u;
                unsigned tid = 0u;
                /* XP+ reservation flag; fallback to 0 on Windows 2000 SP4 */
                uintptr_t th = _beginthreadex(NULL, ARCR_WORKER_STACK_BYTES,
                                              win32_worker_proc, &pool->args[slot],
                                              STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
                if (th == 0u) {
                    tid = 0u;
                    th  = _beginthreadex(NULL, ARCR_WORKER_STACK_BYTES,
                                         win32_worker_proc, &pool->args[slot], 0u, &tid);
                }
                if (th == 0u) {
                    tid = 0u;
                    th  = _beginthreadex(NULL, 0u, win32_worker_proc, &pool->args[slot], 0u, &tid);
                }
                if (th != 0u) {
                    pool->threads[slot] = (HANDLE)th;
                    pool->bg_count++;
                }
            }
        }
        pool->total_workers = 1u + pool->bg_count;
        if (pool->bg_count == 0u) {
            if (pool->wake_event) { CloseHandle(pool->wake_event); pool->wake_event = NULL; }
            if (pool->done_event) { CloseHandle(pool->done_event); pool->done_event = NULL; }
            DeleteCriticalSection(&pool->cs);
        }
    }
    return pool;
}

void arcr_threadpool_destroy(ArcrThreadPool *pool) {
    if (!pool) return;
    if (pool->bg_count > 0u) {
        EnterCriticalSection(&pool->cs);
        pool->shutting_down = 1L;
        SetEvent(pool->wake_event);
        LeaveCriticalSection(&pool->cs);

        WaitForMultipleObjects(pool->bg_count, pool->threads, TRUE, INFINITE);
        for (uint32_t i = 0u; i < pool->bg_count; ++i) {
            if (pool->threads[i]) CloseHandle(pool->threads[i]);
        }
        if (pool->wake_event) CloseHandle(pool->wake_event);
        if (pool->done_event) CloseHandle(pool->done_event);
        DeleteCriticalSection(&pool->cs);
    }
    free(pool);
}

uint32_t arcr_threadpool_worker_count(const ArcrThreadPool *pool) {
    return pool ? pool->total_workers : 1u;
}

void arcr_threadpool_parallel_for(ArcrThreadPool *pool, uint32_t item_count,
                                  ArcrParallelTaskFn fn, void *userdata) {
    if (!fn || item_count == 0u) return;
    if (!pool || pool->bg_count == 0u || item_count == 1u) {
        for (uint32_t i = 0u; i < item_count; ++i) {
            fn(userdata, i, 0u);
        }
        return;
    }

    EnterCriticalSection(&pool->cs);
    pool->item_count = item_count;
    pool->task_fn    = fn;
    pool->task_ud    = userdata;
    pool->next_index = 0L;
    pool->active_bg  = (LONG)pool->bg_count;
    pool->generation++;
    ResetEvent(pool->done_event);
    SetEvent(pool->wake_event);
    LeaveCriticalSection(&pool->cs);

    run_steal_loop(pool, 0u);
    WaitForSingleObject(pool->done_event, INFINITE);
}

#else /* POSIX (Linux / macOS) */

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#define ARCR_WORKER_STACK_BYTES ((size_t)(2u * 1024u * 1024u))

uint32_t arcr_hardware_cpu_threads(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1L) return 1u;
    if (n > 64L) return 64u;
    return (uint32_t)n;
}

typedef struct WorkerArg {
    struct ArcrThreadPool *pool;
    uint32_t               worker_id;
} WorkerArg;

struct ArcrThreadPool {
    uint32_t           total_workers;
    uint32_t           bg_count;
    pthread_t          threads[16];
    WorkerArg          args[16];
    pthread_mutex_t    mtx;
    pthread_cond_t     cv_start;
    pthread_cond_t     cv_done;
    bool               shutting_down;
    uint64_t           generation;
    uint32_t           active_bg;
    atomic_uint        next_index;
    uint32_t           item_count;
    ArcrParallelTaskFn task_fn;
    void              *task_ud;
};

static void run_steal_loop_posix(ArcrThreadPool *pool, uint32_t worker_id) {
    for (;;) {
        uint32_t idx = atomic_fetch_add_explicit(&pool->next_index, 1u, memory_order_relaxed);
        if (idx >= pool->item_count) break;
        pool->task_fn(pool->task_ud, idx, worker_id);
    }
}

static void *posix_worker_proc(void *param) {
    WorkerArg      *wa   = (WorkerArg *)param;
    ArcrThreadPool *pool = wa->pool;
    uint32_t        wid  = wa->worker_id;
    uint64_t        seen_gen = 0u;

    for (;;) {
        pthread_mutex_lock(&pool->mtx);
        while (!pool->shutting_down && pool->generation == seen_gen) {
            pthread_cond_wait(&pool->cv_start, &pool->mtx);
        }
        if (pool->shutting_down) {
            pthread_mutex_unlock(&pool->mtx);
            break;
        }
        seen_gen = pool->generation;
        pthread_mutex_unlock(&pool->mtx);

        run_steal_loop_posix(pool, wid);

        pthread_mutex_lock(&pool->mtx);
        if (--pool->active_bg == 0u) {
            pthread_cond_signal(&pool->cv_done);
        }
        pthread_mutex_unlock(&pool->mtx);
    }
    return NULL;
}

ArcrThreadPool *arcr_threadpool_create(uint32_t desired_threads) {
    ArcrThreadPool *pool = (ArcrThreadPool *)calloc(1u, sizeof(ArcrThreadPool));
    if (!pool) return NULL;

    uint32_t hw = (desired_threads == 0u) ? arcr_hardware_cpu_threads() : desired_threads;
    hw = ARCR_CLAMP(hw, 1u, 16u);
    uint32_t desired_bg = hw - 1u;
    pool->total_workers = 1u;
    pool->bg_count      = 0u;

    if (desired_bg > 0u) {
        if (pthread_mutex_init(&pool->mtx, NULL) != 0) {
            return pool;
        }
        if (pthread_cond_init(&pool->cv_start, NULL) != 0) {
            pthread_mutex_destroy(&pool->mtx);
            return pool;
        }
        if (pthread_cond_init(&pool->cv_done, NULL) != 0) {
            pthread_cond_destroy(&pool->cv_start);
            pthread_mutex_destroy(&pool->mtx);
            return pool;
        }

        pthread_attr_t attr;
        bool has_attr = (pthread_attr_init(&attr) == 0);
        if (has_attr) {
            (void)pthread_attr_setstacksize(&attr, ARCR_WORKER_STACK_BYTES);
        }

        for (uint32_t i = 0u; i < desired_bg; ++i) {
            uint32_t slot = pool->bg_count;
            pool->args[slot].pool      = pool;
            pool->args[slot].worker_id = slot + 1u;
            int rc = pthread_create(&pool->threads[slot], has_attr ? &attr : NULL,
                                    posix_worker_proc, &pool->args[slot]);
            if (rc != 0 && has_attr) {
                rc = pthread_create(&pool->threads[slot], NULL,
                                    posix_worker_proc, &pool->args[slot]);
            }
            if (rc == 0) {
                pool->bg_count++;
            }
        }

        if (has_attr) {
            pthread_attr_destroy(&attr);
        }

        pool->total_workers = 1u + pool->bg_count;
        if (pool->bg_count == 0u) {
            pthread_cond_destroy(&pool->cv_done);
            pthread_cond_destroy(&pool->cv_start);
            pthread_mutex_destroy(&pool->mtx);
        }
    }
    return pool;
}

void arcr_threadpool_destroy(ArcrThreadPool *pool) {
    if (!pool) return;
    if (pool->bg_count > 0u) {
        pthread_mutex_lock(&pool->mtx);
        pool->shutting_down = true;
        pthread_cond_broadcast(&pool->cv_start);
        pthread_mutex_unlock(&pool->mtx);

        for (uint32_t i = 0u; i < pool->bg_count; ++i) {
            pthread_join(pool->threads[i], NULL);
        }
        pthread_cond_destroy(&pool->cv_done);
        pthread_cond_destroy(&pool->cv_start);
        pthread_mutex_destroy(&pool->mtx);
    }
    free(pool);
}

uint32_t arcr_threadpool_worker_count(const ArcrThreadPool *pool) {
    return pool ? pool->total_workers : 1u;
}

void arcr_threadpool_parallel_for(ArcrThreadPool *pool, uint32_t item_count,
                                  ArcrParallelTaskFn fn, void *userdata) {
    if (!fn || item_count == 0u) return;
    if (!pool || pool->bg_count == 0u || item_count == 1u) {
        for (uint32_t i = 0u; i < item_count; ++i) {
            fn(userdata, i, 0u);
        }
        return;
    }

    pthread_mutex_lock(&pool->mtx);
    pool->item_count = item_count;
    pool->task_fn    = fn;
    pool->task_ud    = userdata;
    atomic_store_explicit(&pool->next_index, 0u, memory_order_relaxed);
    pool->active_bg  = pool->bg_count;
    pool->generation++;
    pthread_cond_broadcast(&pool->cv_start);
    pthread_mutex_unlock(&pool->mtx);

    run_steal_loop_posix(pool, 0u);

    pthread_mutex_lock(&pool->mtx);
    while (pool->active_bg > 0u) {
        pthread_cond_wait(&pool->cv_done, &pool->mtx);
    }
    pthread_mutex_unlock(&pool->mtx);
}

#endif
