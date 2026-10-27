/**
 * @file ethernetif.c
 * @brief lwIP netif over the STM32F7 HAL ETH driver (RMII, LAN8742A PHY).
 *
 * Follows the structure of ST's STM32CubeF7 lwIP examples:
 *  - RX is zero-copy. The DMA writes straight into pbufs from RX_POOL, and
 *    an input task feeds them to lwIP when the ETH interrupt signals a frame.
 *  - TX hands the pbuf chain to the DMA and frees it from
 *    HAL_ETH_TxFreeCallback once it has gone out.
 *  - A link task polls the PHY and starts or stops the MAC on link changes.
 *    It also resets the MAC when the RMII receive path comes up broken
 *    (see rx_path_broken()).
 *
 * Checksums are computed and verified by the MAC; frames that fail are
 * dropped in hardware.
 *
 * The data cache stays off, so DMA buffers need no cache maintenance, and
 * every buffer here sits in SRAM1/2, which the Ethernet DMA can reach.
 */

#include <string.h>
#include <stdio.h>
#include <stddef.h>

#include "stm32f7xx_hal.h"
#include "lan8742.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/memp.h"
#include "lwip/tcpip.h"
#include "netif/etharp.h"
#include "ethernetif.h"
#include "lwip/dhcp.h"

#define ETH_RX_BUFFER_SIZE  1536U
/* Received data sits in these buffers (zero-copy) until the application
 * reads it, so they must cover every connection's TCP window: with too few,
 * unread data on idle connections starves the ACKs a blocked sender is
 * waiting for. 6 clients x TCP_WND (4 segments) + 8 spare. */
#define ETH_RX_BUFFER_CNT   32U
#define ETH_TX_TIMEOUT_MS   1000U
#define LINK_POLL_MS        100U
#define RX_SILENCE_MS       10000U /* no frame at all while DHCP is pending */
#define MAC_RESTARTS_MAX    3U     /* then reset the whole chip */

typedef struct {
    struct pbuf_custom pbuf_custom;
    uint8_t buff[(ETH_RX_BUFFER_SIZE + 31U) & ~31U] __attribute__((aligned(32)));
} rx_buff_t;

LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT, sizeof(rx_buff_t), "ETH RX pool");

static ETH_DMADescTypeDef dma_rx_desc[ETH_RX_DESC_CNT] __attribute__((aligned(32)));
static ETH_DMADescTypeDef dma_tx_desc[ETH_TX_DESC_CNT] __attribute__((aligned(32)));

static ETH_HandleTypeDef heth;
static ETH_TxPacketConfigTypeDef tx_config;
static lan8742_Object_t phy;
static SemaphoreHandle_t rx_sem;
static SemaphoreHandle_t tx_sem;
/* Serialises the input task's HAL_ETH_ReadData() with the link task's
 * start, stop and restart. TX runs in the tcpip thread, which the link
 * task excludes with LOCK_TCPIP_CORE(). */
static SemaphoreHandle_t eth_lock;
static volatile int rx_alloc_failed;
static uint8_t mac_addr[6];
static volatile uint32_t rx_frames;

/* ---- PHY bus (MDIO through the MAC) --------------------------------- */

static int32_t phy_io_init(void) {
    HAL_ETH_SetMDIOClockRange(&heth);
    return 0;
}

static int32_t phy_io_deinit(void) { return 0; }

static int32_t phy_io_read(uint32_t addr, uint32_t reg, uint32_t *val) {
    return HAL_ETH_ReadPHYRegister(&heth, addr, reg, val) == HAL_OK ? 0 : -1;
}

static int32_t phy_io_write(uint32_t addr, uint32_t reg, uint32_t val) {
    return HAL_ETH_WritePHYRegister(&heth, addr, reg, val) == HAL_OK ? 0 : -1;
}

static int32_t phy_io_tick(void) { return (int32_t)HAL_GetTick(); }

static lan8742_IOCtx_t phy_io = {
    phy_io_init, phy_io_deinit, phy_io_write, phy_io_read, phy_io_tick
};

/* ---- HAL glue -------------------------------------------------------- */

/* RMII pins on the STM32F769I-DISCO (UM2033):
 * PA1 REF_CLK, PA2 MDIO, PA7 CRS_DV, PC1 MDC, PC4 RXD0, PC5 RXD1,
 * PG11 TX_EN, PG13 TXD0, PG14 TXD1. */
void HAL_ETH_MspInit(ETH_HandleTypeDef *h) {
    (void)h;
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    /* Select RMII with the MAC in reset and its clocks off (RM0410), on
     * every HAL_ETH_Init() including the retries in ethernetif_link_task():
     * this is what gives the MAC's divided RMII clock a new phase. */
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_ETHMAC_FORCE_RESET();
    SYSCFG->PMC &= ~SYSCFG_PMC_MII_RMII_SEL;
    SYSCFG->PMC |= SYSCFG_PMC_MII_RMII_SEL;
    (void)SYSCFG->PMC;
    __HAL_RCC_ETHMAC_RELEASE_RESET();
    __HAL_RCC_ETHMAC_CLK_ENABLE();
    __HAL_RCC_ETHMACTX_CLK_ENABLE();
    __HAL_RCC_ETHMACRX_CLK_ENABLE();

    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF11_ETH;
    gpio.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &gpio);
    gpio.Pin = GPIO_PIN_11 | GPIO_PIN_13 | GPIO_PIN_14;
    HAL_GPIO_Init(GPIOG, &gpio);

    HAL_NVIC_SetPriority(ETH_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(ETH_IRQn);
}

void HAL_ETH_MspDeInit(ETH_HandleTypeDef *h) {
    (void)h;
    HAL_NVIC_DisableIRQ(ETH_IRQn);
    __HAL_RCC_ETHMAC_CLK_DISABLE();
    __HAL_RCC_ETHMACTX_CLK_DISABLE();
    __HAL_RCC_ETHMACRX_CLK_DISABLE();
}

void ETH_IRQHandler(void) {
    HAL_ETH_IRQHandler(&heth);
}

void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *h) {
    (void)h;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(rx_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *h) {
    (void)h;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(tx_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *h) {
    /* Out of RX descriptors: let the input task drain and refill them. */
    if (HAL_ETH_GetDMAError(h) & ETH_DMASR_RBUS) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(rx_sem, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

static void rx_pbuf_free(struct pbuf *p) {
    LWIP_MEMPOOL_FREE(RX_POOL, (struct pbuf_custom *)p);
    if (rx_alloc_failed) {
        rx_alloc_failed = 0;
        xSemaphoreGive(rx_sem);
    }
}

void HAL_ETH_RxAllocateCallback(uint8_t **buff) {
    struct pbuf_custom *p = LWIP_MEMPOOL_ALLOC(RX_POOL);
    if (!p) {
        rx_alloc_failed = 1;
        *buff = NULL;
        return;
    }
    *buff = (uint8_t *)p + offsetof(rx_buff_t, buff);
    p->custom_free_function = rx_pbuf_free;
    pbuf_alloced_custom(PBUF_RAW, 0, PBUF_REF, p, *buff, ETH_RX_BUFFER_SIZE);
}

void HAL_ETH_RxLinkCallback(void **start, void **end, uint8_t *buff, uint16_t len) {
    struct pbuf **head = (struct pbuf **)start;
    struct pbuf **tail = (struct pbuf **)end;
    struct pbuf *p = (struct pbuf *)(buff - offsetof(rx_buff_t, buff));
    p->next = NULL;
    p->tot_len = 0;
    p->len = len;
    if (!*head) {
        *head = p;
    } else {
        (*tail)->next = p;
    }
    *tail = p;
    for (p = *head; p; p = p->next) p->tot_len += len;
}

void HAL_ETH_TxFreeCallback(uint32_t *buff) {
    pbuf_free((struct pbuf *)buff);
}

/* ---- lwIP side -------------------------------------------------------- */

static err_t low_level_output(struct netif *netif, struct pbuf *p) {
    (void)netif;
    ETH_BufferTypeDef bufs[ETH_TX_DESC_CNT];
    memset(bufs, 0, sizeof(bufs));
    unsigned i = 0;
    for (struct pbuf *q = p; q; q = q->next, ++i) {
        if (i >= ETH_TX_DESC_CNT) return ERR_IF;
        bufs[i].buffer = q->payload;
        bufs[i].len = q->len;
        if (i > 0) bufs[i - 1].next = &bufs[i];
    }

    tx_config.Length = p->tot_len;
    tx_config.TxBuffer = bufs;
    tx_config.pData = p;
    pbuf_ref(p); /* dropped by HAL_ETH_TxFreeCallback */

    for (;;) {
        HAL_ETH_ReleaseTxPacket(&heth);
        if (HAL_ETH_Transmit_IT(&heth, &tx_config) == HAL_OK) return ERR_OK;
        /* All descriptors in flight: wait for one to complete, but never
         * for long, since this runs holding the tcpip core lock. */
        if (!(HAL_ETH_GetError(&heth) & HAL_ETH_ERROR_BUSY) ||
            xSemaphoreTake(tx_sem, pdMS_TO_TICKS(ETH_TX_TIMEOUT_MS)) != pdTRUE) {
            pbuf_free(p);
            return ERR_IF;
        }
    }
}

static void input_task(void *arg) {
    struct netif *netif = (struct netif *)arg;
    for (;;) {
        if (xSemaphoreTake(rx_sem, portMAX_DELAY) != pdTRUE) continue;
        for (;;) {
            struct pbuf *p = NULL;
            xSemaphoreTake(eth_lock, portMAX_DELAY);
            HAL_StatusTypeDef st = rx_alloc_failed ? HAL_ERROR : HAL_ETH_ReadData(&heth, (void **)&p);
            xSemaphoreGive(eth_lock);
            if (st != HAL_OK || !p) break;
            rx_frames++;
            if (netif->input(p, netif) != ERR_OK) pbuf_free(p);
        }
    }
}

/* Locally administered MAC derived from the 96-bit device UID. */
static void make_mac(uint8_t mac[6]) {
    uint32_t h = HAL_GetUIDw0() * 2654435761u ^ HAL_GetUIDw1() * 40503u ^ HAL_GetUIDw2();
    mac[0] = 0x02;
    mac[1] = 0x54; /* 'T' */
    mac[2] = (uint8_t)(h >> 24);
    mac[3] = (uint8_t)(h >> 16);
    mac[4] = (uint8_t)(h >> 8);
    mac[5] = (uint8_t)h;
}

err_t ethernetif_init(struct netif *netif) {
    netif->name[0] = 's';
    netif->name[1] = 't';
    netif->output = etharp_output;
    netif->linkoutput = low_level_output;

    make_mac(mac_addr);
    heth.Instance = ETH;
    heth.Init.MACAddr = mac_addr;
    heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
    heth.Init.TxDesc = dma_tx_desc;
    heth.Init.RxDesc = dma_rx_desc;
    heth.Init.RxBuffLen = ETH_RX_BUFFER_SIZE;
    if (HAL_ETH_Init(&heth) != HAL_OK) {
        printf("[eth] HAL_ETH_Init failed (no RMII reference clock?)\n");
        return ERR_IF;
    }

    memset(&tx_config, 0, sizeof(tx_config));
    /* The MAC fills in IP/TCP/UDP/ICMP checksums (lwipopts.h turns lwIP's
     * off). The HAL presets every TX descriptor to full insertion anyway,
     * so software checksums would be overwritten: an ICMP echo reply went
     * out with checksum 0. */
    tx_config.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
    tx_config.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
    tx_config.CRCPadCtrl = ETH_CRC_PAD_INSERT;

    LWIP_MEMPOOL_INIT(RX_POOL);

    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, mac_addr, sizeof(mac_addr));
    netif->mtu = 1500;
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;

    rx_sem = xSemaphoreCreateBinary();
    tx_sem = xSemaphoreCreateBinary();
    eth_lock = xSemaphoreCreateMutex();
    if (!rx_sem || !tx_sem || !eth_lock ||
        xTaskCreate(input_task, "eth_in", 1024, netif, configMAX_PRIORITIES - 2, NULL) != pdPASS) {
        return ERR_MEM;
    }

    LAN8742_RegisterBusIO(&phy, &phy_io);
    if (LAN8742_Init(&phy) != LAN8742_STATUS_OK) {
        printf("[eth] LAN8742 not found on MDIO\n");
        return ERR_IF;
    }
    printf("[eth] MAC %02x:%02x:%02x:%02x:%02x:%02x, PHY at MDIO address %lu\n",
           mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5],
           (unsigned long)phy.DevAddr);
    return ERR_OK;
}

/* On this board the MAC sometimes comes out of reset sampling RMII
 * receive data in the wrong phase: RXD0 reads as 1 on every other REF_CLK
 * edge, so bits 0 and 4 of every byte are set and every frame fails its
 * CRC. It is decided at reset, holds until the next one, and happened on
 * about half the boots whatever the clock source or init order. ST
 * describes the same behaviour as an RMII timing issue between the MAC's
 * RMII clock and its half-rate copy (STM32MP25 erratum ES0598 2.22.2).
 * Each restart of the MAC with its clocks gated draws the phase again, so
 * CRC errors piling up with no frame getting through means restart and
 * try again; after MAC_RESTARTS_MAX the whole chip is reset. A restart has
 * also left the receiver silent (no frames, no errors), so total silence
 * while DHCP is still waiting for its offer counts too. */
static uint32_t last_crc, last_rx, silent_ms;

static int rx_path_broken(struct netif *netif) {
    uint32_t crc = ETH->MMCRFCECR;
    uint32_t rx = rx_frames;
    int broken = crc != last_crc && rx == last_rx;
    if (crc != last_crc || rx != last_rx || dhcp_supplied_address(netif)) {
        silent_ms = 0;
    } else if ((silent_ms += LINK_POLL_MS) >= RX_SILENCE_MS) {
        broken = 1;
    }
    last_crc = crc;
    last_rx = rx;
    return broken;
}

/* Re-initialises the MAC and DMA from clocks-off (HAL_ETH_MspDeInit and
 * HAL_ETH_MspInit). HAL_ETH_Init() rebuilds the descriptors but keeps the
 * handle's bookkeeping (TX buffers in use, a half-assembled RX frame), so
 * every buffer still held goes back first and that state is cleared.
 * Called with the tcpip core lock and eth_lock held. */
static void mac_restart(void) {
    HAL_ETH_Stop_IT(&heth);
    HAL_ETH_ReleaseTxPacket(&heth);
    for (unsigned i = 0; i < ETH_TX_DESC_CNT; ++i) {
        if (heth.TxDescList.PacketAddress[i]) pbuf_free((struct pbuf *)heth.TxDescList.PacketAddress[i]);
    }
    for (unsigned i = 0; i < ETH_RX_DESC_CNT; ++i) {
        if (dma_rx_desc[i].BackupAddr0) {
            pbuf_free((struct pbuf *)(dma_rx_desc[i].BackupAddr0 - offsetof(rx_buff_t, buff)));
        }
    }
    if (heth.RxDescList.pRxStart) pbuf_free((struct pbuf *)heth.RxDescList.pRxStart);
    HAL_ETH_DeInit(&heth);
    memset(&heth.TxDescList, 0, sizeof(heth.TxDescList));
    memset(&heth.RxDescList, 0, sizeof(heth.RxDescList));
    HAL_ETH_Init(&heth);
    last_crc = ETH->MMCRFCECR; /* the reset cleared the MMC counters */
    last_rx = rx_frames;
    silent_ms = 0;
}

void ethernetif_link_task(void *arg) {
    struct netif *netif = (struct netif *)arg;
    unsigned resets = 0;
    for (;;) {
        if (netif_is_link_up(netif) && rx_path_broken(netif)) {
            if (resets == MAC_RESTARTS_MAX) {
                printf("[eth] receive path still broken, resetting the chip\n");
                vTaskDelay(pdMS_TO_TICKS(20)); /* let the console drain */
                NVIC_SystemReset();
            }
            printf("[eth] RMII receive errors, restarting the MAC (%u)\n", ++resets);
            LOCK_TCPIP_CORE();
            xSemaphoreTake(eth_lock, portMAX_DELAY);
            mac_restart();
            xSemaphoreGive(eth_lock);
            netif_set_link_down(netif); /* brought back up below */
            UNLOCK_TCPIP_CORE();
        }

        int32_t state = LAN8742_GetLinkState(&phy);
        int up = state >= LAN8742_STATUS_100MBITS_FULLDUPLEX && state <= LAN8742_STATUS_10MBITS_HALFDUPLEX;

        if (!up && netif_is_link_up(netif)) {
            LOCK_TCPIP_CORE();
            xSemaphoreTake(eth_lock, portMAX_DELAY);
            HAL_ETH_Stop_IT(&heth);
            xSemaphoreGive(eth_lock);
            netif_set_link_down(netif);
            UNLOCK_TCPIP_CORE();
            printf("[eth] link down\n");
        } else if (up && !netif_is_link_up(netif)) {
            ETH_MACConfigTypeDef mac;
            HAL_ETH_GetMACConfig(&heth, &mac);
            mac.Speed = (state == LAN8742_STATUS_100MBITS_FULLDUPLEX ||
                         state == LAN8742_STATUS_100MBITS_HALFDUPLEX) ? ETH_SPEED_100M : ETH_SPEED_10M;
            mac.DuplexMode = (state == LAN8742_STATUS_100MBITS_FULLDUPLEX ||
                              state == LAN8742_STATUS_10MBITS_FULLDUPLEX) ? ETH_FULLDUPLEX_MODE
                                                                          : ETH_HALFDUPLEX_MODE;
            LOCK_TCPIP_CORE();
            xSemaphoreTake(eth_lock, portMAX_DELAY);
            HAL_ETH_SetMACConfig(&heth, &mac);
            HAL_ETH_Start_IT(&heth);
            xSemaphoreGive(eth_lock);
            netif_set_link_up(netif); /* DHCP (re)starts discovery here */
            UNLOCK_TCPIP_CORE();
            printf("[eth] link up, %s %s duplex\n",
                   mac.Speed == ETH_SPEED_100M ? "100 Mbit/s" : "10 Mbit/s",
                   mac.DuplexMode == ETH_FULLDUPLEX_MODE ? "full" : "half");
        }
        vTaskDelay(pdMS_TO_TICKS(LINK_POLL_MS));
    }
}
