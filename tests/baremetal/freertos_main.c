/**
 * @file freertos_main.c
 * @brief Runs a unit test's main() as a FreeRTOS task.
 *
 * Test sources are compiled with -Dmain=ttak_test_main; the scheduler is
 * started here and the test's return value becomes the exit code.
 */

#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "board.h"
#include <ttak/compat/baremetal.h>
#if TTAK_NET_LWIP
#include "lwip/tcpip.h"
#endif

#ifndef TTAK_TEST_TASK_STACK_BYTES
#define TTAK_TEST_TASK_STACK_BYTES (32u * 1024u)
#endif

int ttak_test_main(void);

#if TTAK_NET_LWIP
static void lwip_ready(void *arg) {
    xSemaphoreGive((SemaphoreHandle_t)arg);
}

/* Starts lwIP's tcpip thread; its loopback interface is all the tests use. */
static void start_lwip(void) {
    SemaphoreHandle_t ready = xSemaphoreCreateBinary();
    tcpip_init(lwip_ready, ready);
    xSemaphoreTake(ready, portMAX_DELAY);
    vSemaphoreDelete(ready);
}
#endif

static void test_task(void *param) {
    (void)param;
#if TTAK_NET_LWIP
    start_lwip();
#endif
    board_exit(ttak_test_main());
}

void ttak_rtos_assert_failed(const char *file, int line) {
    printf("[FreeRTOS] assert failed: %s:%d\n", file, line);
    board_exit(96);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    printf("[FreeRTOS] stack overflow in task '%s'\n", name);
    board_exit(95);
}

void vApplicationMallocFailedHook(void) {
    printf("[FreeRTOS] pvPortMalloc failed (heap exhausted)\n");
    board_exit(94);
}

int main(void) {
    if (xTaskCreate(test_task, "test", TTAK_TEST_TASK_STACK_BYTES / sizeof(StackType_t),
                    NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        printf("[FreeRTOS] cannot create test task\n");
        return 93;
    }
    vTaskStartScheduler();
    printf("[FreeRTOS] scheduler returned\n");
    return 92;
}
