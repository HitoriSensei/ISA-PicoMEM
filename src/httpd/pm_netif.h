#pragma once
// lwIP network interface on top of the CYW43 station interface.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lwip/ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts lwIP and the network interface with a static IP. Returns false on failure.
bool pm_netif_init(const uint8_t mac[6], const ip4_addr_t *ip, const ip4_addr_t *mask, const ip4_addr_t *gw);

// Queues a received frame addressed to the PicoMEM IP. Returns true if the frame was taken.
// Must be called from the CYW43 receive callback.
bool pm_netif_rx(const uint8_t *buf, size_t len);

// Processes the received frames and the lwIP timers. Must be called from the main loop.
void pm_netif_poll(void);

bool pm_netif_link_is_up(void);
const ip4_addr_t *pm_netif_ip(void);

#ifdef __cplusplus
}
#endif
