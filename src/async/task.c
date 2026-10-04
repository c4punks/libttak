/**
 * @file task.c
 * @brief Implementation of task creation and execution.
 */

#include <ttak/async/task.h>
#include <ttak/mem/mem.h>
#include <stddef.h>

#include <ttak/async/promise.h>

/**
 * @brief Internal task structure.
 */
struct ttak_task {
    ttak_task_func_t func; /**< Function to execute. */
    void *arg;             /**< Argument for the function. */
    ttak_promise_t *promise; /**< Promise to fulfill. */
    uint64_t task_hash;      /**< Hash to identify task type. */
    uint64_t start_ts;       /**< Execution start timestamp. */
    int base_priority;       /**< Original user priority. */
    uint8_t domain;          /**< Task domain metadata. */
    uint8_t urgency;         /**< Task urgency metadata. */
};

/**
 * @brief Creates a new task.
 * 
 * Allocates memory for the task and initializes it.
 * 
 * @param func Function to execute.
 * @param arg Argument for the function.
 * @param promise Promise to fulfill when the task is complete.
 * @return Pointer to the created task, or NULL on failure.
 */
ttak_task_t *ttak_task_create(ttak_task_func_t func, void *arg, ttak_promise_t *promise, uint64_t now) {
    ttak_task_t *task = (ttak_task_t *)ttak_mem_alloc_raw(sizeof(ttak_task_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (task) {
        task->func = func;
        task->arg = arg;
        task->promise = promise;
        
        // Use a Fibonacci multiply-shift mix: the hash only needs to spread
        // (func, arg) across scheduling shards, so a full SipHash-2-4 round
        // trip is overkill here.
        uintptr_t combined = (uintptr_t)func ^ (uintptr_t)arg;
        task->task_hash = combined * 0x9E3779B97F4A7C15ULL;
        
        task->start_ts = 0;
        task->base_priority = 0;
        task->domain = (uint8_t)TTAK_TASK_DOMAIN_THREAD;
        task->urgency = 0;
    }
    return task;
}

void ttak_task_set_hash(ttak_task_t *task, uint64_t hash) {
    if (task) task->task_hash = hash;
}

uint64_t ttak_task_get_hash(const ttak_task_t *task) {
    return task ? task->task_hash : 0;
}

void ttak_task_set_start_ts(ttak_task_t *task, uint64_t ts) {
    if (task) task->start_ts = ts;
}

uint64_t ttak_task_get_start_ts(const ttak_task_t *task) {
    return task ? task->start_ts : 0;
}

void ttak_task_set_domain(ttak_task_t *task, ttak_task_domain_t domain) {
    if (!task) return;
    task->domain = (uint8_t)domain;
}

ttak_task_domain_t ttak_task_get_domain(const ttak_task_t *task) {
    if (!task) return TTAK_TASK_DOMAIN_UNKNOWN;
    uint8_t domain = task->domain;
    if (domain > (uint8_t)TTAK_TASK_DOMAIN_NET) return TTAK_TASK_DOMAIN_UNKNOWN;
    return (ttak_task_domain_t)domain;
}

void ttak_task_set_urgency(ttak_task_t *task, uint8_t urgency) {
    if (!task) return;
    task->urgency = (urgency > 100U) ? 100U : urgency;
}

uint8_t ttak_task_get_urgency(const ttak_task_t *task) {
    return task ? task->urgency : 0U;
}

/**
 * @brief Executes the task.
 * 
 * @param task Pointer to the task.
 */
void ttak_task_execute(ttak_task_t *task, uint64_t now) {
    if (task && task->func) {
        void *res = task->func(task->arg);
        if (task->promise) {
            ttak_promise_set_value(task->promise, res, now);
        }
    }
}

/**
 * @brief Creates a duplicate of the provided task.
 *
 * @param task Task to duplicate.
 * @param now Current timestamp for memory tracking.
 * @return Pointer to the cloned task or NULL on failure.
 */
ttak_task_t *ttak_task_clone(const ttak_task_t *task, uint64_t now) {
    if (!ttak_mem_access((void *)task, now)) return NULL;
    return ttak_mem_dup_raw(task, sizeof(ttak_task_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
}

/**
 * @brief Destroys the task and frees memory.
 * 
 * @param task Pointer to the task to destroy.
 */
void ttak_task_destroy(ttak_task_t *task, uint64_t now) {
    if (ttak_mem_access(task, now)) {
        ttak_mem_free(task);
    }
}
