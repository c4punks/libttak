/**
 * @file main.c
 * @brief libttak network service on the STM32F769I-DISCO (FreeRTOS + lwIP).
 *
 * Brings the on-board Ethernet up with DHCP, then serves two TCP ports
 * entirely through libttak:
 *
 *  - ECHO_PORT (7000): every chunk a client sends goes to a libttak thread
 *    pool task that upper-cases it; the result is written back. One
 *    poller (ttak_net_poller) multiplexes the listeners and all clients;
 *    reads use ttak_net_view_readv(), writes ttak_io_sync_write().
 *  - STATS_PORT (7001): replies with one line of counters (libttak memory,
 *    buddy pool, newlib and FreeRTOS heaps) and closes.
 *
 * The console (USART1, ST-LINK virtual COM port, 115200 8N1) prints
 * "@@ETH_READY ip=a.b.c.d@@" once DHCP has bound an address;
 * scripts/baremetal/stm32_eth_demo.py waits for it, then drives the tests.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "lwip/tcpip.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "ethernetif.h"

#include <ttak/net/endpoint.h>
#include <ttak/net/poller.h>
#include <ttak/net/view.h>
#include <ttak/io/sync.h>
#include <ttak/mem/mem.h>
#include <ttak/mem/owner.h>
#include <ttak/phys/mem/buddy.h>
#include <ttak/thread/pool.h>
#include <ttak/async/future.h>
#include <ttak/timing/timing.h>
#include "board.h"

#define ECHO_PORT    7000
#define STATS_PORT   7001
#define MAX_CLIENTS  6
#define CHUNK_BYTES  1460

/* ---- Clock, console, HAL time base ----------------------------------- */

/* 25 MHz HSE -> PLL -> 216 MHz SYSCLK (over-drive), APB1 54 MHz, APB2 108 MHz.
 * Ethernet needs HCLK >= 25 MHz. */
static void clock_init(void) {
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM = 25;
    osc.PLL.PLLN = 432;
    osc.PLL.PLLP = RCC_PLLP_DIV2;
    osc.PLL.PLLQ = 9;
    osc.PLL.PLLR = 7;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK || HAL_PWREx_EnableOverDrive() != HAL_OK) {
        board_exit(90);
    }
    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_7) != HAL_OK) {
        board_exit(91);
    }
}

/* lwIP draws DHCP transaction IDs, TCP initial sequence numbers and
 * ephemeral ports from LWIP_RAND() (rand()). Unseeded, every boot reuses
 * the same ones, and a DHCP server may ignore a Discover whose xid it has
 * already answered. Seed from the hardware RNG (48 MHz from PLLQ). */
static void seed_rand(void) {
    __HAL_RCC_RNG_CLK_ENABLE();
    RNG->CR = RNG_CR_RNGEN;
    uint32_t seed = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
    for (int words = 0; words < 4;) {
        uint32_t sr = RNG->SR;
        if (sr & (RNG_SR_SEIS | RNG_SR_CEIS)) {
            RNG->SR = 0;          /* clear the error and retry */
        } else if (sr & RNG_SR_DRDY) {
            seed = seed * 2654435761u ^ RNG->DR;
            ++words;
        }
    }
    RNG->CR = 0;
    srand(seed);
}

/* USART1 on PA9/PA10 (AF7) feeds the ST-LINK virtual COM port. */
void board_init(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &gpio);
    USART1->CR1 = 0;
    USART1->BRR = (HAL_RCC_GetPCLK2Freq() + 115200 / 2) / 115200;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

void board_putc(char c) {
    while (!(USART1->ISR & USART_ISR_TXE)) {
    }
    USART1->TDR = (uint8_t)c;
}

static void board_puts(const char *s) {
    while (*s) board_putc(*s++);
}

void board_exit(int code) {
    char buf[32];
    snprintf(buf, sizeof(buf), "\r\n@@TTAK_EXIT=%d@@\r\n", code);
    board_puts(buf);
    board_halt(code);
}

void board_halt(int code) {
    (void)code;
    while (!(USART1->ISR & USART_ISR_TC)) {
    }
    for (;;) __asm__ volatile("wfi");
}

/* libttak's clock runs on DWT before the scheduler starts and on the
 * FreeRTOS tick after; the HAL borrows it instead of its own SysTick. */
uint32_t ttak_platform_cpu_hz(void) { return SystemCoreClock; }
HAL_StatusTypeDef HAL_InitTick(uint32_t prio) { (void)prio; return HAL_OK; }
uint32_t HAL_GetTick(void) { return (uint32_t)(ttak_platform_now_ns() / 1000000u); }

/* Linked with -nostartfiles: __libc_init_array still calls these. */
void _init(void) {}
void _fini(void) {}

void HardFault_Handler(void) {
    board_puts("\r\n[FAULT] hard fault\r\n");
    board_exit(99);
}

void app_rtos_assert_failed(const char *file, int line) {
    printf("[FreeRTOS] assert failed: %s:%d\n", file, line);
    board_exit(96);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    printf("[FreeRTOS] stack overflow in task '%s'\n", name);
    board_exit(95);
}

void vApplicationMallocFailedHook(void) {
    printf("[FreeRTOS] heap exhausted\n");
    board_exit(94);
}

/* FreeRTOS heap in DTCM (CPU-only memory; the Ethernet DMA never sees it). */
uint8_t ucHeap[configTOTAL_HEAP_SIZE] __attribute__((section(".dtcm"), aligned(8)));

/* ---- The service -------------------------------------------------------- */

typedef struct {
    ttak_shared_net_endpoint_t *ep;
    ttak_io_guard_t io;
    int fd;
    uint32_t bytes;
} client_t;

typedef struct {
    const uint8_t *in;
    uint8_t *out;
    size_t len;
} upper_job_t;

static struct {
    uint32_t accepted;
    uint32_t active;
    uint32_t tasks;
    uint64_t bytes;
} g_stats;

static void *upper_task(void *arg) {
    upper_job_t *job = (upper_job_t *)arg;
    for (size_t i = 0; i < job->len; ++i) {
        uint8_t c = job->in[i];
        job->out[i] = (c >= 'a' && c <= 'z') ? (uint8_t)(c - 'a' + 'A') : c;
    }
    return (void *)(uintptr_t)job->len;
}

static int open_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Endpoints carry no address: closing them must not try to reconnect. */
static ttak_shared_net_endpoint_t *wrap_fd(ttak_owner_t *owner, int fd) {
    uint64_t now = ttak_get_tick_count();
    ttak_shared_net_endpoint_t *ep = ttak_net_endpoint_create(owner, now);
    if (!ep) return NULL;
    if (ttak_net_endpoint_bind_fd(ep, owner, fd, AF_INET, SOCK_STREAM, 0, NULL, 0,
                                  TTAK_NET_ENDPOINT_IPV4, UINT64_MAX, now) != TTAK_IO_SUCCESS) {
        ttak_net_endpoint_destroy(ep, owner, now);
        return NULL;
    }
    return ep;
}

static void send_stats(ttak_owner_t *owner, int fd) {
    ttak_mem_buddy_stats_t buddy;
    ttak_mem_buddy_get_stats(&buddy);
    char *line = ttak_mem_alloc(256, __TTAK_UNSAFE_MEM_FOREVER__, ttak_get_tick_count());
    if (!line) return;
    int n = snprintf(line, 256,
                     "uptime_ms=%lu accepted=%lu active=%lu tasks=%lu bytes=%lu "
                     "ttak_mem=%lu buddy_in_use=%lu buddy_largest_free=%lu "
                     "rtos_heap_free=%lu rtos_heap_min=%lu libc_heap=%lu\n",
                     (unsigned long)(ttak_get_tick_count()), (unsigned long)g_stats.accepted,
                     (unsigned long)g_stats.active, (unsigned long)g_stats.tasks,
                     (unsigned long)g_stats.bytes, (unsigned long)ttak_mem_usage_bytes(),
                     (unsigned long)buddy.bytes_in_use, (unsigned long)buddy.largest_free,
                     (unsigned long)xPortGetFreeHeapSize(),
                     (unsigned long)xPortGetMinimumEverFreeHeapSize(),
                     (unsigned long)mallinfo().uordblks);
    ttak_io_guard_t io;
    uint64_t now = ttak_get_tick_count();
    if (ttak_io_guard_init(&io, fd, owner, UINT64_MAX, now) == TTAK_IO_SUCCESS) {
        size_t written = 0;
        ttak_io_sync_write(&io, line, (size_t)n, &written, now);
    }
    ttak_mem_free(line);
}

static void drop_client(ttak_net_poller_t *poller, ttak_owner_t *owner, client_t *c) {
    uint64_t now = ttak_get_tick_count();
    ttak_net_poller_del(poller, c->ep, owner, now);
    ttak_net_endpoint_destroy(c->ep, owner, now);
    close(c->fd);
    c->ep = NULL;
    c->fd = -1;
    g_stats.active--;
}

/* One chunk: read it, upper-case it on the pool, write it back. Returns
 * false once the peer has closed or failed. */
static bool serve_chunk(ttak_thread_pool_t *pool, ttak_owner_t *owner, client_t *c,
                        uint8_t *in, uint8_t *out) {
    uint64_t now = ttak_get_tick_count();
    ttak_net_view_iovec_t iov = { in, CHUNK_BYTES };
    size_t got = 0;
    if (ttak_net_view_readv(c->ep, owner, &iov, 1, &got, 0, now) != TTAK_IO_SUCCESS) {
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    if (got == 0) return false;

    upper_job_t job = { in, out, got };
    ttak_future_t *f = ttak_thread_pool_submit_task(pool, upper_task, &job, 0, now);
    if (!f) return false;
    size_t done = (size_t)(uintptr_t)ttak_future_get(f);
    ttak_future_release(f);
    g_stats.tasks++;

    size_t written = 0;
    if (ttak_io_sync_write(&c->io, out, done, &written, now) != TTAK_IO_SUCCESS || written != done) {
        return false;
    }
    c->bytes += (uint32_t)done;
    g_stats.bytes += done;
    return true;
}

static void service_task(void *arg) {
    (void)arg;
    ttak_owner_t *owner = ttak_owner_create(TTAK_OWNER_SAFE_DEFAULT);
    ttak_thread_pool_t *pool = ttak_thread_pool_create(2, 0, ttak_get_tick_count());
    uint8_t *in = ttak_mem_alloc(CHUNK_BYTES, __TTAK_UNSAFE_MEM_FOREVER__, ttak_get_tick_count());
    uint8_t *out = ttak_mem_alloc(CHUNK_BYTES, __TTAK_UNSAFE_MEM_FOREVER__, ttak_get_tick_count());
    int echo_fd = open_listener(ECHO_PORT);
    int stats_fd = open_listener(STATS_PORT);
    ttak_shared_net_endpoint_t *echo_ep = owner ? wrap_fd(owner, echo_fd) : NULL;
    ttak_shared_net_endpoint_t *stats_ep = owner ? wrap_fd(owner, stats_fd) : NULL;
    if (!owner || !pool || !in || !out || !echo_ep || !stats_ep) {
        printf("[app] service setup failed\n");
        board_exit(2);
    }

    ttak_net_poller_t poller;
    uint64_t now = ttak_get_tick_count();
    ttak_net_poller_init(&poller, MAX_CLIENTS + 2, now);
    ttak_net_poller_add(&poller, echo_ep, TTAK_NET_EVENT_READ, &echo_fd, owner, now);
    ttak_net_poller_add(&poller, stats_ep, TTAK_NET_EVENT_READ, &stats_fd, owner, now);

    client_t clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; ++i) clients[i].fd = -1;
    printf("[app] echo on %d, stats on %d (libttak poller + %d pool workers)\n",
           ECHO_PORT, STATS_PORT, 2);

    for (;;) {
        ttak_net_poller_event_t evs[MAX_CLIENTS + 2];
        int n = ttak_net_poller_wait(&poller, evs, MAX_CLIENTS + 2, 1000, ttak_get_tick_count());
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(10)); /* e.g. out of memory: back off, don't spin */
            continue;
        }
        for (int e = 0; e < n; ++e) {
            void *tag = evs[e].user_data;
            if (tag == &stats_fd) {
                int fd = accept(stats_fd, NULL, NULL);
                if (fd >= 0) {
                    send_stats(owner, fd);
                    close(fd);
                }
            } else if (tag == &echo_fd) {
                int fd = accept(echo_fd, NULL, NULL);
                if (fd < 0) continue;
                client_t *c = NULL;
                for (int i = 0; i < MAX_CLIENTS && !c; ++i) {
                    if (clients[i].fd < 0) c = &clients[i];
                }
                now = ttak_get_tick_count();
                if (!c || !(c->ep = wrap_fd(owner, fd)) ||
                    ttak_io_guard_init(&c->io, fd, owner, UINT64_MAX, now) != TTAK_IO_SUCCESS ||
                    ttak_net_poller_add(&poller, c->ep, TTAK_NET_EVENT_READ, c, owner, now) != TTAK_IO_SUCCESS) {
                    if (c && c->ep) ttak_net_endpoint_destroy(c->ep, owner, now);
                    if (c) c->ep = NULL;
                    close(fd);
                    continue;
                }
                c->fd = fd;
                c->bytes = 0;
                g_stats.accepted++;
                g_stats.active++;
            } else {
                client_t *c = (client_t *)tag;
                if (c->fd >= 0 && !serve_chunk(pool, owner, c, in, out)) {
                    drop_client(&poller, owner, c);
                }
            }
        }
    }
}

/* ---- Bring-up --------------------------------------------------------- */

static struct netif g_netif;

static void netif_status(struct netif *netif) {
    if (netif_is_up(netif) && !ip4_addr_isany_val(*netif_ip4_addr(netif))) {
        char addr[16], gw[16];
        ip4addr_ntoa_r(netif_ip4_addr(netif), addr, sizeof(addr));
        ip4addr_ntoa_r(netif_ip4_gw(netif), gw, sizeof(gw));
        printf("[net] address %s, gateway %s\n", addr, gw);
    }
}

static void tcpip_ready(void *arg) {
    xSemaphoreGive((SemaphoreHandle_t)arg);
}

static void app_task(void *arg) {
    (void)arg;
    SemaphoreHandle_t ready = xSemaphoreCreateBinary();
    tcpip_init(tcpip_ready, ready);
    xSemaphoreTake(ready, portMAX_DELAY);

    LOCK_TCPIP_CORE();
    struct netif *ok = netif_add(&g_netif, NULL, NULL, NULL, NULL, ethernetif_init, tcpip_input);
    if (ok) {
        netif_set_default(&g_netif);
        netif_set_status_callback(&g_netif, netif_status);
        /* Administratively up from the start; the link task follows the
         * cable. dhcp_start() refuses a netif that is down. */
        netif_set_up(&g_netif);
        dhcp_start(&g_netif);
    }
    UNLOCK_TCPIP_CORE();
    if (!ok) {
        printf("[net] Ethernet bring-up failed\n");
        board_exit(3);
    }
    xTaskCreate(ethernetif_link_task, "eth_link", 512, &g_netif, 2, NULL);

    /* Wait for the DHCP lease. */
    for (int waited = 0; !dhcp_supplied_address(&g_netif); waited += 100) {
        if (waited == 60000) {
            printf("[net] no DHCP lease after 60 s (cable plugged in?)\n");
            board_exit(4);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    printf("@@ETH_READY ip=%s@@\n", ip4addr_ntoa(netif_ip4_addr(&g_netif)));

    xTaskCreate(service_task, "service", 2048, NULL, 3, NULL);
    vTaskDelete(NULL);
}

int main(void) {
    HAL_Init();
    clock_init();
    SCB_EnableICache(); /* the D-cache stays off: Ethernet DMA buffers */
    seed_rand();
    board_init();       /* console; after clock_init() for the 108 MHz APB2 baud base */
    printf("\n[app] libttak on STM32F769I-DISCO, %lu MHz, FreeRTOS + lwIP\n",
           (unsigned long)(SystemCoreClock / 1000000u));

    xTaskCreate(app_task, "app", 2048, NULL, 3, NULL);
    vTaskStartScheduler();
    return 1;
}
