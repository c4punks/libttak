/* lwIP netif for the STM32F769I-DISCO's Ethernet MAC and LAN8742A PHY. */
#ifndef ETHERNETIF_H
#define ETHERNETIF_H

#include "lwip/err.h"
#include "lwip/netif.h"

/* netif_add() init callback: brings up the MAC and PHY. */
err_t ethernetif_init(struct netif *netif);

/* Polls the PHY and follows link changes (speed, duplex, up/down).
 * Run it as a task with the netif as argument. */
void ethernetif_link_task(void *netif);

#endif /* ETHERNETIF_H */
