/**
 * @file pthread.h
 * @brief POSIX thread compatibility shim.
 *
 * On non-MSVC compilers (GCC, Clang, TCC) this header forwards to the
 * system-provided <pthread.h> via #include_next so that no existing code
 * needs modification.
 *
 * On MSVC/Windows, the system does not ship a pthread implementation.
 * This file supplies a minimal but correct compatibility layer built on
 * top of the Windows synchronisation API (SRWLOCK, CONDITION_VARIABLE,
 * CRITICAL_SECTION, CreateThread / WaitForSingleObject, InitOnceExecuteOnce).
 *
 * On WASI (preview1), the single-threaded host has no usable pthread
 * implementation. Sysroots that ship their own <pthread.h> (e.g.
 * wasi-libc, whose pthread types are also pulled in by <sys/types.h>)
 * are preferred; otherwise a self-contained single-threaded stub set
 * is provided as a fallback.
 */

#if !defined(_MSC_VER) || !defined(_WIN32)
#  if defined(__has_include_next)
#    if __has_include_next(<pthread.h>)
#      include_next <pthread.h>
#      define __TTAK_PTHREAD_SYSTEM 1
#    endif
#  else
#    include_next <pthread.h>
#    define __TTAK_PTHREAD_SYSTEM 1
#  endif
#endif

#ifndef TTAK_PTHREAD_SHIM_H
#define TTAK_PTHREAD_SHIM_H

#if defined(__wasi__) && !defined(__TTAK_PTHREAD_SYSTEM)
/* -----------------------------------------------------------------------
 * WASI (preview1) fallback: the sysroot ships no <pthread.h>. Searching
 * host include directories would leak glibc headers, so this branch
 * provides self-contained stubs. Synchronisation primitives cannot
 * contend on a single-threaded host: mutexes track state only to catch
 * misuse, cond vars never block, and pthread_create fails with ENOSYS.
 *
 * Note: sysroots whose <sys/types.h> defines pthread types (musl-style
 * bits/alltypes.h, as in wasi-libc) cannot use this fallback - the
 * typedefs would collide. Such sysroots are expected to provide their
 * own <pthread.h>, which is consumed via include_next above.
 * --------------------------------------------------------------------- */

#include <errno.h>   /* EBUSY / EDEADLK / ENOSYS */
#include <stddef.h>  /* size_t */

typedef int pthread_mutex_t;
typedef int pthread_cond_t;
typedef int pthread_t;
typedef int pthread_once_t;
typedef int pthread_attr_t;
typedef int pthread_mutexattr_t;
typedef int pthread_condattr_t;
typedef int pthread_rwlockattr_t;
typedef struct { int exclusive; } pthread_rwlock_t;

#define PTHREAD_MUTEX_INITIALIZER 0
#define PTHREAD_ONCE_INIT         0
#define PTHREAD_RWLOCK_INITIALIZER   { 0 }
#define PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP  1  /* stub value */

static __inline int pthread_mutex_init(pthread_mutex_t *m,
                                        const pthread_mutexattr_t *attr) {
    (void)attr; *m = 0; return 0;
}
static __inline int pthread_mutex_lock(pthread_mutex_t *m) {
    if (*m) return EDEADLK;
    *m = 1; return 0;
}
static __inline int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (*m) return EBUSY;
    *m = 1; return 0;
}
static __inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    *m = 0; return 0;
}
static __inline int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m; return 0;
}

static __inline int pthread_attr_init(pthread_attr_t *attr) {
    (void)attr; return 0;
}
static __inline int pthread_attr_destroy(pthread_attr_t *attr) {
    (void)attr; return 0;
}
static __inline int pthread_attr_setstacksize(pthread_attr_t *attr,
                                              size_t stacksize) {
    (void)attr; (void)stacksize; return 0;
}
static __inline int pthread_attr_getstacksize(const pthread_attr_t *attr,
                                              size_t *stacksize) {
    (void)attr; if (stacksize) *stacksize = 0; return 0;
}

static __inline int pthread_cond_init(pthread_cond_t *c,
                                       const pthread_condattr_t *attr) {
    (void)c; (void)attr; return 0;
}
static __inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    /* Single-threaded host: never block; the caller re-checks state. */
    (void)c; (void)m; return 0;
}
static __inline int pthread_cond_timedwait(pthread_cond_t *c,
                                            pthread_mutex_t *m,
                                            const struct timespec *abstime) {
    (void)c; (void)m; (void)abstime; return 0;
}
static __inline int pthread_cond_signal(pthread_cond_t *c) {
    (void)c; return 0;
}
static __inline int pthread_cond_broadcast(pthread_cond_t *c) {
    (void)c; return 0;
}
static __inline int pthread_cond_destroy(pthread_cond_t *c) {
    (void)c; return 0;
}

static __inline int pthread_rwlock_init(pthread_rwlock_t *rw,
                                         const pthread_rwlockattr_t *attr) {
    (void)attr; rw->exclusive = 0; return 0;
}
static __inline int pthread_rwlock_rdlock(pthread_rwlock_t *rw) {
    if (rw->exclusive) return EDEADLK;
    return 0;
}
static __inline int pthread_rwlock_wrlock(pthread_rwlock_t *rw) {
    if (rw->exclusive) return EDEADLK;
    rw->exclusive = 1; return 0;
}
static __inline int pthread_rwlock_unlock(pthread_rwlock_t *rw) {
    rw->exclusive = 0; return 0;
}
static __inline int pthread_rwlock_destroy(pthread_rwlock_t *rw) {
    (void)rw; return 0;
}

static __inline int pthread_rwlockattr_init(pthread_rwlockattr_t *a) {
    (void)a; return 0;
}
static __inline int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a) {
    (void)a; return 0;
}
static __inline int pthread_rwlockattr_setkind_np(pthread_rwlockattr_t *a,
                                                   int pref) {
    (void)a; (void)pref; return 0;
}

static __inline int pthread_once(pthread_once_t *once_ctrl,
                                  void (*init_routine)(void)) {
    /* States mirror the MSVC branch: 0=new, 1=running, 2=done. A second
     * call cannot arrive mid-initialisation on a single-threaded host. */
    if (*once_ctrl != 2) {
        *once_ctrl = 1;
        init_routine();
        *once_ctrl = 2;
    }
    return 0;
}

static __inline int pthread_create(pthread_t *thread,
                                    const void *attr,
                                    void *(*start_routine)(void *),
                                    void *arg) {
    (void)attr; (void)start_routine; (void)arg;
    if (thread) *thread = 0;
    return ENOSYS;
}
static __inline int pthread_join(pthread_t thread, void **retval) {
    (void)thread; (void)retval; return 0;
}
static __inline pthread_t pthread_self(void) {
    return 0;
}

#endif /* __wasi__ && !__TTAK_PTHREAD_SYSTEM */

#if defined(_MSC_VER) && defined(_WIN32)
/* -----------------------------------------------------------------------
 * MSVC / Windows implementation
 * --------------------------------------------------------------------- */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>  /* _beginthreadex */
#include <stdlib.h>   /* malloc / free  */
#include <time.h>     /* struct timespec, timespec_get */
#include <errno.h>    /* ETIMEDOUT / EINVAL */
#include <limits.h>   /* UINT_MAX */

/* --- types --- */

typedef SRWLOCK             pthread_mutex_t;
typedef CONDITION_VARIABLE  pthread_cond_t;
typedef HANDLE              pthread_t;
typedef volatile LONG       pthread_once_t;
typedef struct {
    size_t stack_size;
} pthread_attr_t;

/* pthread_rwlock_t: wrap SRWLOCK with a flag to distinguish shared vs
 * exclusive mode so that a single pthread_rwlock_unlock() can work.     */
typedef struct {
    SRWLOCK          lock;
    volatile int     exclusive; /* 1 when write-locked, 0 otherwise */
} pthread_rwlock_t;

typedef int pthread_mutexattr_t;   /* unused stub */
typedef int pthread_condattr_t;    /* unused stub */
typedef int pthread_rwlockattr_t;  /* unused stub */

/* --- static-initialiser macros --- */

#define PTHREAD_MUTEX_INITIALIZER   SRWLOCK_INIT   /* {0} */
#define PTHREAD_ONCE_INIT           0L
#define PTHREAD_RWLOCK_INITIALIZER   { SRWLOCK_INIT, 0 }
#define PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP  1  /* stub value */

/* --- mutex --- */

static __inline int pthread_mutex_init(pthread_mutex_t *m,
                                        const pthread_mutexattr_t *attr) {
    (void)attr;
    InitializeSRWLock(m);
    return 0;
}
static __inline int pthread_mutex_lock(pthread_mutex_t *m) {
    AcquireSRWLockExclusive(m);
    return 0;
}
static __inline int pthread_mutex_trylock(pthread_mutex_t *m) {
    return TryAcquireSRWLockExclusive(m) ? 0 : 16 /* EBUSY */;
}
static __inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    ReleaseSRWLockExclusive(m);
    return 0;
}
static __inline int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m; /* SRWLOCK requires no cleanup */
    return 0;
}

/* --- thread attributes --- */

static __inline int pthread_attr_init(pthread_attr_t *attr) {
    if (!attr) return EINVAL;
    attr->stack_size = 0;
    return 0;
}

static __inline int pthread_attr_destroy(pthread_attr_t *attr) {
    (void)attr;
    return 0;
}

static __inline int pthread_attr_setstacksize(pthread_attr_t *attr,
                                              size_t stacksize) {
    if (!attr) return EINVAL;
    attr->stack_size = stacksize;
    return 0;
}

static __inline int pthread_attr_getstacksize(const pthread_attr_t *attr,
                                              size_t *stacksize) {
    if (!attr || !stacksize) return EINVAL;
    *stacksize = attr->stack_size;
    return 0;
}

/* --- condition variable --- */

static __inline int pthread_cond_init(pthread_cond_t *c,
                                       const pthread_condattr_t *attr) {
    (void)attr;
    InitializeConditionVariable(c);
    return 0;
}
static __inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    /* mutex is an SRWLOCK held in exclusive mode */
    return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : (int)GetLastError();
}
static __inline int pthread_cond_timedwait(pthread_cond_t *c,
                                            pthread_mutex_t *m,
                                            const struct timespec *abstime) {
    /* Maximum safe millisecond value for SleepConditionVariableSRW
     * (one less than INFINITE = 0xFFFFFFFF to avoid the sentinel value). */
    static const DWORD k_max_sleep_ms = 0xFFFFFFFEUL;
    DWORD timeout_ms = INFINITE;
    if (abstime) {
        struct timespec now;
        timespec_get(&now, TIME_UTC);
        long long diff_ns =
            ((long long)(abstime->tv_sec  - now.tv_sec)  * 1000000000LL) +
            ((long long) abstime->tv_nsec - (long long)now.tv_nsec);
        if (diff_ns <= 0) {
            timeout_ms = 0;
        } else {
            long long diff_ms = (diff_ns + 999999LL) / 1000000LL;
            timeout_ms = (diff_ms >= (long long)k_max_sleep_ms)
                             ? k_max_sleep_ms
                             : (DWORD)diff_ms;
        }
    }
    if (SleepConditionVariableSRW(c, m, timeout_ms, 0)) return 0;
    return (GetLastError() == ERROR_TIMEOUT) ? ETIMEDOUT : (int)GetLastError();
}
static __inline int pthread_cond_signal(pthread_cond_t *c) {
    WakeConditionVariable(c);
    return 0;
}
static __inline int pthread_cond_broadcast(pthread_cond_t *c) {
    WakeAllConditionVariable(c);
    return 0;
}
static __inline int pthread_cond_destroy(pthread_cond_t *c) {
    (void)c; /* CONDITION_VARIABLE requires no cleanup */
    return 0;
}

/* --- read-write lock --- */

static __inline int pthread_rwlock_init(pthread_rwlock_t *rw,
                                         const pthread_rwlockattr_t *attr) {
    (void)attr;
    InitializeSRWLock(&rw->lock);
    rw->exclusive = 0;
    return 0;
}
static __inline int pthread_rwlock_rdlock(pthread_rwlock_t *rw) {
    AcquireSRWLockShared(&rw->lock);
    return 0;
}
static __inline int pthread_rwlock_wrlock(pthread_rwlock_t *rw) {
    AcquireSRWLockExclusive(&rw->lock);
    rw->exclusive = 1;
    return 0;
}
static __inline int pthread_rwlock_unlock(pthread_rwlock_t *rw) {
    if (rw->exclusive) {
        rw->exclusive = 0;
        ReleaseSRWLockExclusive(&rw->lock);
    } else {
        ReleaseSRWLockShared(&rw->lock);
    }
    return 0;
}
static __inline int pthread_rwlock_destroy(pthread_rwlock_t *rw) {
    (void)rw; /* SRWLOCK requires no cleanup */
    return 0;
}

/* rwlock attribute stubs (glibc extension used in detachable.c) */
static __inline int pthread_rwlockattr_init(pthread_rwlockattr_t *a) {
    (void)a; return 0;
}
static __inline int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a) {
    (void)a; return 0;
}
static __inline int pthread_rwlockattr_setkind_np(pthread_rwlockattr_t *a,
                                                   int pref) {
    (void)a; (void)pref; return 0;
}

/* --- one-time initialisation --- */

static __inline int pthread_once(pthread_once_t *once_ctrl,
                                  void (*init_routine)(void)) {
    /* State: 0=not started, 1=in progress, 2=done */
    if (*once_ctrl == 2L) return 0;
    if (InterlockedCompareExchange(once_ctrl, 1L, 0L) == 0L) {
        init_routine();
        InterlockedExchange(once_ctrl, 2L);
    } else {
        while (*once_ctrl != 2L)
            SwitchToThread();
    }
    return 0;
}

/* --- thread creation / join --- */

/* Helper to bridge void*(*)(void*) to unsigned __stdcall (*)(void*) */
typedef struct {
    void *(*func)(void *);
    void *arg;
} __ttak_pthread_start_t;

static unsigned __stdcall __ttak_pthread_entry(void *raw) {
    __ttak_pthread_start_t *s = (__ttak_pthread_start_t *)raw;
    void *(*f)(void *) = s->func;
    void *a            = s->arg;
    free(s);
    f(a);
    return 0;
}

static __inline int pthread_create(pthread_t *thread,
                                    const void *attr,
                                    void *(*start_routine)(void *),
                                    void *arg) {
    __ttak_pthread_start_t *s;
    unsigned win_stack_sz = 0;
    if (attr) {
        const pthread_attr_t *a = (const pthread_attr_t *)attr;
        if (a->stack_size > 0) {
            win_stack_sz = (a->stack_size > (size_t)UINT_MAX)
                               ? UINT_MAX
                               : (unsigned)a->stack_size;
        }
    }
    s = ((__ttak_pthread_start_t *)malloc(sizeof(*s)));
    if (!s) return 12 /* ENOMEM */;
    s->func   = start_routine;
    s->arg    = arg;
    *thread = (HANDLE)_beginthreadex(NULL, win_stack_sz,
                                     __ttak_pthread_entry, s, 0, NULL);
    if (!*thread) { free(s); return (int)GetLastError(); }
    return 0;
}

static __inline int pthread_join(pthread_t thread, void **retval) {
    (void)retval;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}

static __inline pthread_t pthread_self(void) {
    return GetCurrentThread();
}

#endif /* _MSC_VER && _WIN32 */

#endif /* TTAK_PTHREAD_SHIM_H */
