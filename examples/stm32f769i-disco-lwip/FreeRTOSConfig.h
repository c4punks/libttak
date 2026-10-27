/*
 * FreeRTOS configuration for the STM32F769I-DISCO lwIP example. libttak
 * is compiled against this file; the settings marked "libttak" are what
 * src/platform/freertos.c requires.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

extern uint32_t SystemCoreClock;

#define configCPU_CLOCK_HZ                       (SystemCoreClock)
#define configTICK_RATE_HZ                       1000
#define configUSE_PREEMPTION                     1
#define configUSE_TIME_SLICING                   1
#define configMAX_PRIORITIES                     8
#define configMINIMAL_STACK_SIZE                 256
#define configMAX_TASK_NAME_LEN                  16
#define configTICK_TYPE_WIDTH_IN_BITS            TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                  1

/* The heap lives in DTCM (see stm32f769i_disco_eth.ld): task stacks and
 * kernel objects only, never Ethernet DMA buffers. */
#define configSUPPORT_DYNAMIC_ALLOCATION         1   /* libttak */
#define configSUPPORT_STATIC_ALLOCATION          0
#define configAPPLICATION_ALLOCATED_HEAP         1
#define configTOTAL_HEAP_SIZE                    (112 * 1024)

#define configUSE_MUTEXES                        1   /* libttak */
#define configUSE_RECURSIVE_MUTEXES              1
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS  1   /* libttak */
#define configTASK_NOTIFICATION_ARRAY_ENTRIES    2   /* libttak */
#define configUSE_TASK_NOTIFICATIONS             1

#define TTAK_FREERTOS_THREAD_STACK_BYTES         (8u * 1024u)

#define configUSE_NEWLIB_REENTRANT               1   /* libttak */

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
#define INCLUDE_vTaskDelay                       1   /* libttak, lwIP */
#define INCLUDE_vTaskSuspend                     1   /* libttak, lwIP */
#define INCLUDE_vTaskPrioritySet                 1

/* 4 priority bits on STM32F7. The ETH interrupt runs at 6, below
 * configMAX_SYSCALL_INTERRUPT_PRIORITY, so it may give semaphores. */
#define configPRIO_BITS                          4
#define configKERNEL_INTERRUPT_PRIORITY          (15 << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     (5 << (8 - configPRIO_BITS))

void app_rtos_assert_failed(const char *file, int line);
#define configASSERT(x) do { if (!(x)) app_rtos_assert_failed(__FILE__, __LINE__); } while (0)

#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
