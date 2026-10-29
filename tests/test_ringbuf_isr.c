/**
 * @file test_ringbuf_isr.c
 * @brief ISR-safe ring buffer API tests (issue #105).
 *
 * Hosted: the critical-section macros are no-ops, so the _isr variants
 * run unlocked; the smoke test covers the API and ordering single-threaded.
 *
 * Cortex-M (bare-metal / FreeRTOS, QEMU mps2-an500): NVIC IRQ0 is enabled
 * and pended; the ISR pushes payloads with ttak_ringbuf_push_isr() while
 * the task pops with ttak_ringbuf_pop_isr() and checks order and loss.
 */

#include <ttak/container/ringbuf.h>
#include <ttak/arch/ttak_critical.h>
#include "test_macros.h"

#if defined(TTAK_ARCH_CORTEX_M) && TTAK_PLATFORM_BAREMETAL
#  include <stdint.h>
#  define TTAK_HAVE_CORTEX_M_ISR_TEST 1
#else
#  define TTAK_HAVE_CORTEX_M_ISR_TEST 0
#endif

#define ISR_TEST_ITEMS 200
#define ISR_TEST_CAPACITY 16

#if TTAK_HAVE_CORTEX_M_ISR_TEST

/* NVIC registers (matches the MMIO style in board_mps2_an500.c). */
#define NVIC_ISER0 (*(volatile uint32_t *)0xE000E100u)
#define NVIC_ISPR0 (*(volatile uint32_t *)0xE000E200u)
#define NVIC_IPR0  (*(volatile uint32_t *)(0xE000E400u))

static ttak_ringbuf_t g_rb;
static uint8_t g_rb_storage[TTAK_RINGBUF_STATIC_STORAGE_SIZE(ISR_TEST_CAPACITY, sizeof(uint32_t))];

/* ISR-written, task-read. */
static volatile uint32_t g_isr_next;
static volatile uint32_t g_isr_pushed;

/* TTAK_IRQ0_Handler overrides the weak alias in startup_cortex_m.c. */
/* Pushes stop at ISR_TEST_ITEMS: a firing that delivers the last payload
 * must not run over, or g_isr_pushed overshoots the exact count the task
 * asserts. */
void TTAK_IRQ0_Handler(void) {
    for (int i = 0; i < 4; ++i) {
        if (g_isr_next >= ISR_TEST_ITEMS) return;
        uint32_t v = g_isr_next;
        if (ttak_ringbuf_push_isr(&g_rb, &v)) {
            g_isr_next = v + 1;
            g_isr_pushed++;
        }
    }
}

static void test_isr_producer_consumer(void) {
    ASSERT(ttak_ringbuf_init_static(&g_rb, ISR_TEST_CAPACITY, sizeof(uint32_t),
                                    g_rb_storage, sizeof(g_rb_storage)));
    g_isr_next = 0;
    g_isr_pushed = 0;

    NVIC_IPR0 = 0x80u;
    NVIC_ISER0 = 1u << 0;

    uint32_t expected = 0;
    uint32_t idle = 0;
    while (expected < ISR_TEST_ITEMS) {
        NVIC_ISPR0 = 1u << 0;
        uint32_t out;
        if (ttak_ringbuf_pop_isr(&g_rb, &out)) {
            ASSERT_MSG(out == expected,
                       "order violation: got %lu, want %lu",
                       (unsigned long)out, (unsigned long)expected);
            expected++;
            idle = 0;
        } else if (++idle > 10000000u) {
            ASSERT_MSG(false, "stalled at %lu of %lu (pushed %lu)",
                       (unsigned long)expected, (unsigned long)ISR_TEST_ITEMS,
                       (unsigned long)g_isr_pushed);
        }
    }
    ASSERT(g_isr_pushed == ISR_TEST_ITEMS);
    uint32_t out;
    ASSERT(!ttak_ringbuf_pop_isr(&g_rb, &out));
}

#endif /* TTAK_HAVE_CORTEX_M_ISR_TEST */

static void test_isr_api_smoke_host(void) {
    /* Critical sections are no-ops here; the _isr variants run unlocked. */
    static uint8_t storage[TTAK_RINGBUF_STATIC_STORAGE_SIZE(8, sizeof(int))];
    ttak_ringbuf_t rb;
    ASSERT(ttak_ringbuf_init_static(&rb, 8, sizeof(int), storage, sizeof(storage)));

    ASSERT(ttak_ringbuf_is_empty(&rb));
    for (int i = 0; i < 8; ++i) {
        ASSERT(ttak_ringbuf_push_isr(&rb, &i));
    }
    ASSERT(!ttak_ringbuf_push_isr(&rb, &(int){0}));
    for (int i = 0; i < 8; ++i) {
        int out = -1;
        ASSERT(ttak_ringbuf_pop_isr(&rb, &out));
        ASSERT(out == i);
    }
    ASSERT(!ttak_ringbuf_pop_isr(&rb, &(int){0}));
    ASSERT(ttak_ringbuf_is_empty(&rb));
}

int main(void) {
#if TTAK_HAVE_CORTEX_M_ISR_TEST
    RUN_TEST(test_isr_producer_consumer);
#endif
    RUN_TEST(test_isr_api_smoke_host);
    return 0;
}
