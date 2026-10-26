/*
 * FreeRTOS configuration for the libttak bare-metal test harness
 * (Cortex-M4F/M7 port). Applications use their own FreeRTOSConfig.h; the
 * settings marked "libttak" are what src/platform/freertos.c requires.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

uint32_t ttak_platform_cpu_hz(void);

#define configCPU_CLOCK_HZ                       (ttak_platform_cpu_hz())
#define configTICK_RATE_HZ                       1000
#define configUSE_PREEMPTION                     1
#define configUSE_TIME_SLICING                   1
#define configMAX_PRIORITIES                     8
#define configMINIMAL_STACK_SIZE                 256
#define configMAX_TASK_NAME_LEN                  16
#define configTICK_TYPE_WIDTH_IN_BITS            TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                  1

/* Memory: heap_4 backs kernel objects and libttak thread records. */
#define configSUPPORT_DYNAMIC_ALLOCATION         1   /* libttak */
#define configSUPPORT_STATIC_ALLOCATION          0
#define configTOTAL_HEAP_SIZE                    (192 * 1024)

/* libttak: mutexes, per-task thread record, cond-var wakeups. */
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              1
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS  1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES    2
#define configUSE_TASK_NOTIFICATIONS             1

/* libttak worker stacks come from the heap above (default 16 KiB each);
 * 8 KiB is ample for the tests. Stack overflow checking stays on. */
#define TTAK_FREERTOS_THREAD_STACK_BYTES         (8u * 1024u)

/* Per-task newlib state so printf/errno are task-safe. */
#define configUSE_NEWLIB_REENTRANT               1

#define configUSE_COUNTING_SEMAPHORES            1
#define configQUEUE_REGISTRY_SIZE                0
#define configUSE_TIMERS                         0
#define configUSE_CO_ROUTINES                    0
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_MALLOC_FAILED_HOOK             1

#define INCLUDE_vTaskDelete                      1   /* libttak */
#define INCLUDE_xTaskGetSchedulerState           1   /* libttak */
#define INCLUDE_xTaskGetCurrentTaskHandle        1   /* libttak */
#define INCLUDE_uxTaskPriorityGet                1   /* libttak */
#define INCLUDE_vTaskDelay                       1   /* libttak */
#define INCLUDE_vTaskSuspend                     1   /* libttak */
#define INCLUDE_vTaskPrioritySet                 1

/* Interrupt priorities: 4 priority bits on STM32F7. */
#define configPRIO_BITS                          4
#define configKERNEL_INTERRUPT_PRIORITY          (15 << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     (5 << (8 - configPRIO_BITS))

void ttak_rtos_assert_failed(const char *file, int line);
#define configASSERT(x) do { if (!(x)) ttak_rtos_assert_failed(__FILE__, __LINE__); } while (0)

/* Map the port's handlers onto the vector table names. */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
