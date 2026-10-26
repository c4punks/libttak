/**
 * @file freertos.c
 * @brief pthread, clock and sleep backend on top of FreeRTOS.
 *
 * Built when TTAK_PLATFORM_FREERTOS=1 (Makefile: PLATFORM=freertos). The
 * application's FreeRTOSConfig.h must provide:
 *
 *  - configUSE_MUTEXES 1 and configSUPPORT_DYNAMIC_ALLOCATION 1
 *  - configNUM_THREAD_LOCAL_STORAGE_POINTERS >= 1 (slot
 *    TTAK_FREERTOS_TLS_INDEX, default 0, holds the thread record)
 *  - configTASK_NOTIFICATION_ARRAY_ENTRIES >= 2 (index
 *    TTAK_FREERTOS_NOTIFY_INDEX, default the last one, wakes cond waiters)
 *  - INCLUDE_vTaskDelete, INCLUDE_vTaskSuspend, INCLUDE_xTaskGetSchedulerState,
 *    INCLUDE_xTaskGetCurrentTaskHandle, INCLUDE_uxTaskPriorityGet
 *
 * TTAK_FREERTOS_THREAD_STACK_BYTES (default 16 KiB),
 * TTAK_FREERTOS_THREAD_PRIORITY, TTAK_FREERTOS_TLS_INDEX and
 * TTAK_FREERTOS_NOTIFY_INDEX may be set in FreeRTOSConfig.h.
 *
 * Kernel objects behind pthread_mutex_t / pthread_cond_t / pthread_rwlock_t
 * are created on first use, so static PTHREAD_*_INITIALIZER values work.
 * Before vTaskStartScheduler() only one context runs, so locking is a
 * no-op and condition waits return immediately.
 *
 * newlib's malloc is made task-safe through __malloc_lock/__malloc_unlock;
 * stdio reentrancy needs configUSE_NEWLIB_REENTRANT.
 */

#include <ttak/compat/pthread.h>

#if TTAK_PLATFORM_FREERTOS

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "../../internal/ttak/platform_internal.h"

#ifndef TTAK_FREERTOS_TLS_INDEX
#define TTAK_FREERTOS_TLS_INDEX 0
#endif
#ifndef TTAK_FREERTOS_NOTIFY_INDEX
#define TTAK_FREERTOS_NOTIFY_INDEX (configTASK_NOTIFICATION_ARRAY_ENTRIES - 1)
#endif
#ifndef TTAK_FREERTOS_THREAD_STACK_BYTES
#define TTAK_FREERTOS_THREAD_STACK_BYTES (16u * 1024u)
#endif
#ifndef TTAK_FREERTOS_THREAD_PRIORITY
#define TTAK_FREERTOS_THREAD_PRIORITY (tskIDLE_PRIORITY + 1)
#endif

#if configNUM_THREAD_LOCAL_STORAGE_POINTERS <= TTAK_FREERTOS_TLS_INDEX
#error "libttak needs configNUM_THREAD_LOCAL_STORAGE_POINTERS > TTAK_FREERTOS_TLS_INDEX"
#endif
#if !defined(configTASK_NOTIFICATION_ARRAY_ENTRIES) || configTASK_NOTIFICATION_ARRAY_ENTRIES < 2
#error "libttak needs configTASK_NOTIFICATION_ARRAY_ENTRIES >= 2"
#endif
#if TTAK_FREERTOS_NOTIFY_INDEX == 0
#error "TTAK_FREERTOS_NOTIFY_INDEX 0 is reserved for application task notifications"
#endif
#if !configUSE_MUTEXES || !configSUPPORT_DYNAMIC_ALLOCATION
#error "libttak needs configUSE_MUTEXES and configSUPPORT_DYNAMIC_ALLOCATION"
#endif

_Static_assert(sizeof(void *) <= sizeof(pthread_mutex_t), "mutex word must hold a handle");
_Static_assert(sizeof(void *) <= sizeof(pthread_cond_t), "cond word must hold a pointer");
_Static_assert(sizeof(void *) <= sizeof(pthread_t), "pthread_t must hold a pointer");

#define TTAK_FREERTOS_TICK_NS (1000000000ull / (uint64_t)configTICK_RATE_HZ)

static inline bool scheduler_running(void) {
    return xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

/* Critical sections for bookkeeping that may also run before the
 * scheduler starts. There is only one context then, and entering a
 * FreeRTOS critical section would keep interrupts masked until
 * vTaskStartScheduler(). */
static inline bool crit_enter(void) {
    if (!scheduler_running()) return false;
    taskENTER_CRITICAL();
    return true;
}

static inline void crit_exit(bool entered) {
    if (entered) taskEXIT_CRITICAL();
}

/* Install a lazily created object into a zero-initialised word. Returns
 * the installed value; the caller frees its candidate if it lost a race. */
static uintptr_t lazy_install(volatile uintptr_t *slot, uintptr_t candidate) {
    bool crit = crit_enter();
    if (*slot == 0) *slot = candidate;
    uintptr_t cur = *slot;
    crit_exit(crit);
    return cur;
}

/* ------------------------------------------------------------------------
 * Clock and sleep
 * ---------------------------------------------------------------------- */

#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)
#define SCB_ICSR  (*(volatile uint32_t *)0xE000ED04u)
#define ICSR_PENDSTSET (1u << 26)

static uint64_t g_clock_base_ns;  /* hardware time when ticks took over */
static uint64_t g_clock_last_ns;
static uint64_t g_tick_hi;
static TickType_t g_tick_last;
static bool g_clock_ticks;

/* Once the scheduler runs, time comes from the FreeRTOS tick count plus
 * the SysTick position within the current tick: it cannot miss counter
 * wraps however long the system idles. Before that, the hardware clock
 * is used; the switch-over and suspended-scheduler windows are clamped
 * so the result never goes backwards. */
__attribute__((weak)) uint64_t ttak_platform_now_ns(void) {
    if (!scheduler_running()) {
        return ttak_bm_hw_now_ns();
    }
    UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
    if (!g_clock_ticks) {
        g_clock_base_ns = ttak_bm_hw_now_ns();
        g_clock_ticks = true;
    }
    TickType_t ticks = xTaskGetTickCountFromISR();
    uint32_t reload = SYST_RVR + 1u;
    uint32_t val = SYST_CVR;
    if (SCB_ICSR & ICSR_PENDSTSET) {
        /* SysTick wrapped but the tick interrupt has not run yet. */
        val = SYST_CVR;
        ticks++;
    }
    if (ticks < g_tick_last) g_tick_hi += (uint64_t)1 << (8 * sizeof(TickType_t));
    g_tick_last = ticks;
    uint64_t sub = ((uint64_t)(reload - 1u - val) * TTAK_FREERTOS_TICK_NS) / reload;
    uint64_t ns = g_clock_base_ns + (g_tick_hi + ticks) * TTAK_FREERTOS_TICK_NS + sub;
    if (ns < g_clock_last_ns) ns = g_clock_last_ns;
    g_clock_last_ns = ns;
    portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
    return ns;
}

/* Ticks covering at least ns: vTaskDelay(n) can return up to one tick
 * early, so add one. */
static TickType_t ns_to_delay_ticks(uint64_t ns) {
    uint64_t ticks = (ns + TTAK_FREERTOS_TICK_NS - 1) / TTAK_FREERTOS_TICK_NS + 1;
    return ticks >= portMAX_DELAY ? portMAX_DELAY - 1 : (TickType_t)ticks;
}

int nanosleep(const struct timespec *rqtp, struct timespec *rmtp) {
    if (!rqtp || rqtp->tv_nsec < 0 || rqtp->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    uint64_t ns = (uint64_t)rqtp->tv_sec * 1000000000ull + (uint64_t)rqtp->tv_nsec;
    if (scheduler_running()) {
        vTaskDelay(ns_to_delay_ticks(ns));
    } else {
        ttak_bm_spin_ns(ns);
    }
    if (rmtp) {
        rmtp->tv_sec = 0;
        rmtp->tv_nsec = 0;
    }
    return 0;
}

int sched_yield(void) {
    if (scheduler_running()) taskYIELD();
    return 0;
}

/* ------------------------------------------------------------------------
 * newlib heap lock
 * ---------------------------------------------------------------------- */

struct _reent;

void __malloc_lock(struct _reent *r) {
    (void)r;
    if (scheduler_running()) vTaskSuspendAll();
}

void __malloc_unlock(struct _reent *r) {
    (void)r;
    if (scheduler_running()) (void)xTaskResumeAll();
}

/* ------------------------------------------------------------------------
 * Threads and thread-specific data
 * ---------------------------------------------------------------------- */

#ifndef TTAK_PTHREAD_KEYS_MAX
#define TTAK_PTHREAD_KEYS_MAX TTAK_PTHREAD_STUB_KEYS_MAX
#endif
#define TTAK_PTHREAD_DESTRUCTOR_ITERATIONS 4

typedef struct ttak_rtos_thread {
    void *(*start)(void *);
    void *arg;
    void *ret;
    TaskHandle_t task;
    SemaphoreHandle_t done;   /* given once the thread has finished */
    bool detached;
    bool exited;
    void *keys[TTAK_PTHREAD_KEYS_MAX];
} ttak_rtos_thread_t;

static void (*g_key_dtors[TTAK_PTHREAD_KEYS_MAX])(void *);
static bool g_key_used[TTAK_PTHREAD_KEYS_MAX];

/* Record for the single pre-scheduler context. */
static ttak_rtos_thread_t g_boot_thread = { .detached = true };

/* Tasks not started through pthread_create() get a record on first use.
 * It is never freed: FreeRTOS has no hook for a foreign task's deletion. */
static ttak_rtos_thread_t *current_thread(void) {
    if (!scheduler_running()) return &g_boot_thread;
    ttak_rtos_thread_t *self =
        (ttak_rtos_thread_t *)pvTaskGetThreadLocalStoragePointer(NULL, TTAK_FREERTOS_TLS_INDEX);
    if (!self) {
        self = (ttak_rtos_thread_t *)pvPortMalloc(sizeof(*self));
        if (!self) return &g_boot_thread;
        memset(self, 0, sizeof(*self));
        self->detached = true;
        vTaskSetThreadLocalStoragePointer(NULL, TTAK_FREERTOS_TLS_INDEX, self);
    }
    return self;
}

static void run_key_destructors(ttak_rtos_thread_t *self) {
    for (int round = 0; round < TTAK_PTHREAD_DESTRUCTOR_ITERATIONS; ++round) {
        bool again = false;
        for (unsigned k = 0; k < TTAK_PTHREAD_KEYS_MAX; ++k) {
            void *value = self->keys[k];
            void (*dtor)(void *) = g_key_dtors[k];
            if (value && dtor) {
                self->keys[k] = NULL;
                dtor(value);
                again = true;
            }
        }
        if (!again) break;
    }
}

static void free_thread(ttak_rtos_thread_t *t) {
    if (t->done) vSemaphoreDelete(t->done);
    vPortFree(t);
}

static void thread_trampoline(void *param) {
    ttak_rtos_thread_t *self = (ttak_rtos_thread_t *)param;
    vTaskSetThreadLocalStoragePointer(NULL, TTAK_FREERTOS_TLS_INDEX, self);
    self->ret = self->start(self->arg);
    run_key_destructors(self);

    taskENTER_CRITICAL();
    self->exited = true;
    bool detached = self->detached;
    taskEXIT_CRITICAL();
    vTaskSetThreadLocalStoragePointer(NULL, TTAK_FREERTOS_TLS_INDEX, NULL);
    if (detached) {
        free_thread(self);
        vTaskDelete(NULL); /* stack reclaimed later by the idle task */
    }
    /* Joinable: park until pthread_join() deletes this task. Deleting it
     * from another task frees the stack at once instead of waiting for
     * the idle task, which may not run while busy threads hold the CPU. */
    xSemaphoreGive(self->done);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

int pthread_create(pthread_t *thread, const void *attr,
                   void *(*start_routine)(void *), void *arg) {
    (void)attr;
    if (!thread || !start_routine) return EINVAL;
    ttak_rtos_thread_t *t = (ttak_rtos_thread_t *)pvPortMalloc(sizeof(*t));
    if (!t) return EAGAIN;
    memset(t, 0, sizeof(*t));
    t->start = start_routine;
    t->arg = arg;
    t->done = xSemaphoreCreateBinary();
    if (!t->done) {
        vPortFree(t);
        return EAGAIN;
    }
    /* Inherit the creator's priority, as POSIX threads do by default. */
    UBaseType_t prio = scheduler_running() ? uxTaskPriorityGet(NULL)
                                           : TTAK_FREERTOS_THREAD_PRIORITY;
    configSTACK_DEPTH_TYPE depth =
        (configSTACK_DEPTH_TYPE)(TTAK_FREERTOS_THREAD_STACK_BYTES / sizeof(StackType_t));
    if (xTaskCreate(thread_trampoline, "ttak", depth, t, prio, &t->task) != pdPASS) {
        free_thread(t);
        return EAGAIN;
    }
    *thread = (pthread_t)(uintptr_t)t;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    ttak_rtos_thread_t *t = (ttak_rtos_thread_t *)(uintptr_t)thread;
    if (!t || t->detached) return EINVAL;
    if (t == current_thread()) return EDEADLK;
    xSemaphoreTake(t->done, portMAX_DELAY);
    vTaskDelete(t->task);
    if (retval) *retval = t->ret;
    free_thread(t);
    return 0;
}

int pthread_detach(pthread_t thread) {
    ttak_rtos_thread_t *t = (ttak_rtos_thread_t *)(uintptr_t)thread;
    if (!t) return EINVAL;
    bool crit = crit_enter();
    bool already = t->detached;
    bool exited = t->exited;
    t->detached = true;
    crit_exit(crit);
    if (already) return EINVAL;
    if (exited) {
        /* It finished as a joinable thread and is parking: reap it. */
        xSemaphoreTake(t->done, portMAX_DELAY);
        vTaskDelete(t->task);
        free_thread(t);
    }
    return 0;
}

pthread_t pthread_self(void) {
    return (pthread_t)(uintptr_t)current_thread();
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    bool crit = crit_enter();
    for (unsigned k = 0; k < TTAK_PTHREAD_KEYS_MAX; ++k) {
        if (!g_key_used[k]) {
            g_key_used[k] = true;
            g_key_dtors[k] = destructor;
            crit_exit(crit);
            *key = (pthread_key_t)k;
            return 0;
        }
    }
    crit_exit(crit);
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= TTAK_PTHREAD_KEYS_MAX) return EINVAL;
    bool crit = crit_enter();
    g_key_used[key] = false;
    g_key_dtors[key] = NULL;
    crit_exit(crit);
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    if (key >= TTAK_PTHREAD_KEYS_MAX) return NULL;
    return current_thread()->keys[key];
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    if (key >= TTAK_PTHREAD_KEYS_MAX) return EINVAL;
    current_thread()->keys[key] = (void *)value;
    return 0;
}

int pthread_attr_init(pthread_attr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
    (void)attr;
    return 0;
}

/* Stack size is fixed by TTAK_FREERTOS_THREAD_STACK_BYTES. */
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize) {
    (void)attr;
    (void)stacksize;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize) {
    (void)attr;
    if (stacksize) *stacksize = TTAK_FREERTOS_THREAD_STACK_BYTES;
    return 0;
}

int pthread_once(pthread_once_t *once_ctrl, void (*init_routine)(void)) {
    /* init_executed: 0 = new, 1 = running, 2 = done. */
    volatile int *state = &once_ctrl->init_executed;
    bool crit = crit_enter();
    int prev = *state;
    if (prev == 0) *state = 1;
    crit_exit(crit);
    if (prev == 0) {
        init_routine();
        *state = 2;
        return 0;
    }
    while (*state != 2) {
        if (scheduler_running()) vTaskDelay(1);
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Mutexes
 * ---------------------------------------------------------------------- */

static SemaphoreHandle_t mutex_handle(pthread_mutex_t *m) {
    uintptr_t h = *(volatile pthread_mutex_t *)m;
    if (h) return (SemaphoreHandle_t)h;
    SemaphoreHandle_t fresh = xSemaphoreCreateMutex();
    if (!fresh) return NULL;
    h = lazy_install((volatile uintptr_t *)m, (uintptr_t)fresh);
    if (h != (uintptr_t)fresh) vSemaphoreDelete(fresh);
    return (SemaphoreHandle_t)h;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr) {
    (void)attr;
    *m = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    if (!scheduler_running()) return 0;
    SemaphoreHandle_t h = mutex_handle(m);
    if (!h) return ENOMEM;
    xSemaphoreTake(h, portMAX_DELAY);
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (!scheduler_running()) return 0;
    SemaphoreHandle_t h = mutex_handle(m);
    if (!h) return ENOMEM;
    return xSemaphoreTake(h, 0) == pdTRUE ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (!scheduler_running()) return 0;
    SemaphoreHandle_t h = (SemaphoreHandle_t)*(volatile pthread_mutex_t *)m;
    if (!h) return EPERM;
    return xSemaphoreGive(h) == pdTRUE ? 0 : EPERM;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    SemaphoreHandle_t h = (SemaphoreHandle_t)*m;
    *m = 0;
    if (h) vSemaphoreDelete(h);
    return 0;
}

/* ------------------------------------------------------------------------
 * Wait queues
 *
 * Blocked tasks queue a node on their own stack and sleep on a dedicated
 * task-notification index. A waker dequeues nodes inside a critical
 * section and notifies afterwards. A task waits on one queue at a time,
 * so the index is shared by condition variables and rwlocks.
 * ---------------------------------------------------------------------- */

typedef struct waiter {
    struct waiter *next;
    TaskHandle_t task;
} waiter_t;

typedef struct {
    waiter_t *head;
    waiter_t *tail;
} waitq_t;

/* Callers hold a critical section for the queue operations below. */
static void waitq_push(waitq_t *q, waiter_t *w) {
    w->next = NULL;
    if (q->tail) q->tail->next = w;
    else q->head = w;
    q->tail = w;
}

static TaskHandle_t waitq_pop(waitq_t *q) {
    waiter_t *w = q->head;
    if (!w) return NULL;
    q->head = w->next;
    if (!q->head) q->tail = NULL;
    return w->task;
}

static waiter_t *waitq_take_all(waitq_t *q) {
    waiter_t *w = q->head;
    q->head = q->tail = NULL;
    return w;
}

static bool waitq_unlink(waitq_t *q, waiter_t *w) {
    waiter_t *prev = NULL;
    for (waiter_t *it = q->head; it; prev = it, it = it->next) {
        if (it != w) continue;
        if (prev) prev->next = it->next;
        else q->head = it->next;
        if (q->tail == it) q->tail = prev;
        return true;
    }
    return false;
}

static void wake(TaskHandle_t task) {
    if (task) xTaskNotifyGiveIndexed(task, TTAK_FREERTOS_NOTIFY_INDEX);
}

/* Wake a detached chain; read each link before notifying, because the
 * node lives on the waiter's stack and is gone once the waiter runs. */
static void wake_all(waiter_t *w) {
    while (w) {
        waiter_t *next = w->next;
        wake(w->task);
        w = next;
    }
}

static void sleep_until_woken(void) {
    ulTaskNotifyTakeIndexed(TTAK_FREERTOS_NOTIFY_INDEX, pdTRUE, portMAX_DELAY);
}

/* ------------------------------------------------------------------------
 * Condition variables
 *
 * A waiter that times out removes its node; if a signaller already took
 * it, the waiter consumes the in-flight notification so the node outlives
 * every reference to it.
 * ---------------------------------------------------------------------- */

static waitq_t *cond_queue(pthread_cond_t *c) {
    uintptr_t p = *(volatile pthread_cond_t *)c;
    if (p) return (waitq_t *)p;
    waitq_t *fresh = (waitq_t *)pvPortMalloc(sizeof(*fresh));
    if (!fresh) return NULL;
    fresh->head = fresh->tail = NULL;
    p = lazy_install((volatile uintptr_t *)c, (uintptr_t)fresh);
    if (p != (uintptr_t)fresh) vPortFree(fresh);
    return (waitq_t *)p;
}

static int cond_wait_ticks(pthread_cond_t *c, pthread_mutex_t *m, TickType_t ticks) {
    if (!scheduler_running()) return 0; /* single context: spurious wakeup */
    waitq_t *q = cond_queue(c);
    if (!q) return ENOMEM;

    waiter_t self = { NULL, xTaskGetCurrentTaskHandle() };
    taskENTER_CRITICAL();
    waitq_push(q, &self);
    taskEXIT_CRITICAL();

    pthread_mutex_unlock(m);
    uint32_t woken = ulTaskNotifyTakeIndexed(TTAK_FREERTOS_NOTIFY_INDEX, pdTRUE, ticks);
    if (!woken) {
        taskENTER_CRITICAL();
        bool still_queued = waitq_unlink(q, &self);
        taskEXIT_CRITICAL();
        if (!still_queued) {
            sleep_until_woken();
            woken = 1;
        }
    }
    pthread_mutex_lock(m);
    return woken ? 0 : ETIMEDOUT;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr) {
    (void)attr;
    *c = 0;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    return cond_wait_ticks(c, m, portMAX_DELAY);
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abstime) {
    if (!abstime) return EINVAL;
    uint64_t deadline = (uint64_t)abstime->tv_sec * 1000000000ull + (uint64_t)abstime->tv_nsec;
    uint64_t now = ttak_platform_now_ns(); /* CLOCK_REALTIME == CLOCK_MONOTONIC here */
    if (deadline <= now) return ETIMEDOUT;
    return cond_wait_ticks(c, m, ns_to_delay_ticks(deadline - now));
}

int pthread_cond_signal(pthread_cond_t *c) {
    waitq_t *q = (waitq_t *)*(volatile pthread_cond_t *)c;
    if (!q) return 0;
    bool crit = crit_enter();
    TaskHandle_t task = waitq_pop(q);
    crit_exit(crit);
    wake(task);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    waitq_t *q = (waitq_t *)*(volatile pthread_cond_t *)c;
    if (!q) return 0;
    bool crit = crit_enter();
    waiter_t *all = waitq_take_all(q);
    crit_exit(crit);
    wake_all(all);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) {
    waitq_t *q = (waitq_t *)*c;
    *c = 0;
    if (q) vPortFree(q);
    return 0;
}

/* ------------------------------------------------------------------------
 * Reader-writer locks (writer-preferring)
 *
 * Uncontended acquire and release are a single critical section. Blocked
 * tasks queue and are woken by the releaser; each re-checks the state, so
 * a reader that loses the race to a newly arrived writer queues again.
 * ---------------------------------------------------------------------- */

typedef struct {
    waitq_t readers_q;
    waitq_t writers_q;
    unsigned readers;
    unsigned writers_waiting;
    bool writer;
} rwlock_impl_t;

static rwlock_impl_t *rwlock_impl(pthread_rwlock_t *rw) {
    volatile uintptr_t *slot = (volatile uintptr_t *)&TTAK_PTHREAD_STUB_RW(rw);
    uintptr_t p = *slot;
    if (p) return (rwlock_impl_t *)p;
    rwlock_impl_t *fresh = (rwlock_impl_t *)pvPortMalloc(sizeof(*fresh));
    if (!fresh) return NULL;
    memset(fresh, 0, sizeof(*fresh));
    p = lazy_install(slot, (uintptr_t)fresh);
    if (p != (uintptr_t)fresh) vPortFree(fresh);
    return (rwlock_impl_t *)p;
}

int pthread_rwlock_init(pthread_rwlock_t *rw, const pthread_rwlockattr_t *attr) {
    (void)attr;
    TTAK_PTHREAD_STUB_RW(rw) = 0;
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rw) {
    rwlock_impl_t *l = rwlock_impl(rw);
    if (!l) return ENOMEM;
    waiter_t self = { NULL, NULL };
    for (;;) {
        bool crit = crit_enter();
        if (!l->writer && !l->writers_waiting) {
            l->readers++;
            crit_exit(crit);
            return 0;
        }
        if (!crit) return EDEADLK; /* single context cannot wait */
        self.task = xTaskGetCurrentTaskHandle();
        waitq_push(&l->readers_q, &self);
        crit_exit(crit);
        sleep_until_woken();
    }
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rw) {
    rwlock_impl_t *l = rwlock_impl(rw);
    if (!l) return ENOMEM;
    waiter_t self = { NULL, NULL };
    bool counted = false;
    for (;;) {
        bool crit = crit_enter();
        if (!l->writer && !l->readers) {
            if (counted) l->writers_waiting--;
            l->writer = true;
            crit_exit(crit);
            return 0;
        }
        if (!crit) return EDEADLK; /* single context cannot wait */
        if (!counted) {
            l->writers_waiting++;
            counted = true;
        }
        self.task = xTaskGetCurrentTaskHandle();
        waitq_push(&l->writers_q, &self);
        crit_exit(crit);
        sleep_until_woken();
    }
}

int pthread_rwlock_unlock(pthread_rwlock_t *rw) {
    rwlock_impl_t *l = rwlock_impl(rw);
    if (!l) return ENOMEM;
    TaskHandle_t writer = NULL;
    waiter_t *readers = NULL;
    bool crit = crit_enter();
    if (l->writer) l->writer = false;
    else if (l->readers) l->readers--;
    if (!l->readers) {
        if (l->writers_waiting) writer = waitq_pop(&l->writers_q);
        else readers = waitq_take_all(&l->readers_q);
    }
    crit_exit(crit);
    wake(writer);
    wake_all(readers);
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rw) {
    rwlock_impl_t *l = (rwlock_impl_t *)TTAK_PTHREAD_STUB_RW(rw);
    TTAK_PTHREAD_STUB_RW(rw) = 0;
    if (l) vPortFree(l);
    return 0;
}

int pthread_rwlockattr_init(pthread_rwlockattr_t *a) {
    (void)a;
    return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a) {
    (void)a;
    return 0;
}

int pthread_rwlockattr_setkind_np(pthread_rwlockattr_t *a, int pref) {
    (void)a;
    (void)pref;
    return 0;
}

#endif /* TTAK_PLATFORM_FREERTOS */
